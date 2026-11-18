from __future__ import annotations

from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass, field
import json
import logging
from pathlib import Path
from threading import Event, RLock
from time import monotonic
from uuid import uuid4

from backend.blueprints.bindings import BindingResolver
from backend.blueprints.graph import Graph, graph_for, node_test_graph, validate_ports, validate_request
from backend.blueprints.order import order_clause, price_order
from backend.blueprints.runtime import TorchBlueprintSession, check_runtime
from backend.blueprints.schemas import (
    AdapterNode, Blueprint, DatasetNode, GenerationNode, InlineRows, Issue, NodeTestRequest,
    Problems, Revision, RunRequest, RunSnapshot,
    RunStatus, RunSummary, RunSurface, Step, StepStatus, ToolNode, checked_json,
)
from backend.blueprints.store import BlueprintStore, now
from backend.inference.engines.base import BaseEngine
from backend.inference.supervisor import get_supervisor
from backend.paths import app_home
from backend.tooling import pi_bridge
from backend.tooling.actions import resolve_directory
from backend.tooling.approvals import ToolBroker

logger = logging.getLogger(__name__)
_MAX_ACTIVE = 8
_MAX_TRACE_BYTES = 32 * 1024**2


class RunnerUnavailable(RuntimeError):
    pass


@dataclass
class _Job:
    run: RunSummary
    snapshot: RunSnapshot
    graph: Graph
    steps: dict[str, Step]
    tool_engine: BaseEngine | None = None
    stop: Event = field(default_factory=Event)
    step_started: float | None = None


def _json(value) -> str:
    return json.dumps(value, ensure_ascii=False, allow_nan=False, separators=(",", ":"))


def _transform(node, value):
    match node.operation:
        case "pass":
            return value
        case "parse-json":
            try:
                return checked_json(json.loads(value))
            except (ValueError, RecursionError) as exc:
                raise ValueError(f"Cannot parse JSON: {exc}") from exc
        case "field":
            name = node.parameters["field"]
            if name not in value:
                raise ValueError(f"Object has no field '{name}'")
            return value[name]
        case "first":
            if not value:
                raise ValueError("Cannot get the first item of an empty array")
            return value[0]
        case "count":
            return len(value)
        case "format":
            return value if isinstance(value, str) else _json(value)
    raise RuntimeError(f"Unsupported transform '{node.operation}'")


def _execute(node, inputs, snapshot: RunSnapshot, stop: Event, session):
    if node.kind == "input":
        return {"value": snapshot.request.inputs[node.id]}
    if isinstance(node, DatasetNode):
        data = node.binding
        rows = data.rows if isinstance(data, InlineRows) else snapshot.bindings.datasets[data.dataset_id].rows
        return {"rows": rows}
    if node.kind == "output":
        return {"value": inputs["value"]}
    if isinstance(node, AdapterNode):
        return {"adapter": node.adapter_id}
    if node.kind == "transform":
        if node.operation == "constant":
            return {"value": node.parameters["value"]}
        if node.operation == "build-object":
            return {"value": inputs}
        if node.operation == "order-clause":
            return {"value": order_clause(inputs["message"], inputs["index"])}
        if node.operation == "order-price":
            return {"value": price_order(inputs["clause"], inputs["fields"])}
        return {"value": _transform(node, inputs["value"])}
    if isinstance(node, GenerationNode) and session is not None:
        return session.generate(node, inputs, snapshot.bindings, stop)
    fixture = snapshot.request.fixtures[node.id]
    stop.wait(fixture.delay_ms / 1000)
    return fixture.outputs


def _finished_step(step: Step, status: StepStatus, at, duration: float | None,
                   issue: Issue | None = None) -> Step:
    return step.model_copy(update={"status": status, "finished_at": at,
                                   "duration_ms": duration, "error": issue})


def _terminal_steps(steps, status, at, issue=None, elapsed=None):
    changed = []
    for step in steps:
        if step.status == "pending":
            changed.append(_finished_step(step, "skipped", at, None))
        elif step.status == "running":
            duration = elapsed if elapsed is not None else max(0, (at - step.started_at).total_seconds() * 1000)
            step_issue = issue.model_copy(update={"node_id": step.node_id}) if issue else None
            changed.append(_finished_step(step, status, at, duration, step_issue))
    return changed


class BlueprintRunner:
    def __init__(self, store: BlueprintStore, resolver: BindingResolver):
        self.store = store
        self.resolver = resolver
        self.broker = ToolBroker(store.path)
        self._lock = RLock()
        self._jobs: dict[str, _Job] = {}
        self._closing = False
        self._working = False
        self._recover()
        self._executor = ThreadPoolExecutor(max_workers=1, thread_name_prefix="blueprint")

    def _recover(self) -> None:
        for run, steps in self.store.interrupted():
            active = next((s.node_id for s in steps if s.status == "running"), None)
            issue = Issue(code="interrupted", node_id=active, message="Backend stopped before the run finished")
            at = now()
            terminal = run.model_copy(update={"status": "failed", "finished_at": at, "error": issue})
            self.store.transition(terminal, _terminal_steps(steps, "failed", at, issue), run_event=True)

    def validate(self, blueprint: Blueprint) -> None:
        self.resolver.resolve(graph_for(blueprint))

    def capabilities(self, blueprint: Blueprint) -> dict:
        graph = graph_for(blueprint)
        runtime = check_runtime(graph, self.resolver.resolve(graph))
        engine = self._tool_engine(graph)
        if engine is not None:
            runtime["pi"] = {"engine": engine.name, "model": engine.model_ref}
        return runtime

    @staticmethod
    def _tool_engine(graph: Graph) -> BaseEngine | None:
        for node in graph.nodes.values():
            if not isinstance(node, ToolNode):
                continue
            kind = node.inputs["value"].type
            if node.operation == "approval" and kind != "object":
                raise Problems([Issue(code="invalid_tool_input", node_id=node.id,
                                      message="A local approval node requires an object input")])
            if node.operation == "agent" and kind not in ("string", "array"):
                raise Problems([Issue(code="invalid_tool_input", node_id=node.id,
                                      message="A Pi agent node requires text or chat messages")])
        agent = next((n for n in graph.nodes.values() if isinstance(n, ToolNode) and n.operation == "agent"), None)
        if agent is None:
            return None
        try:
            return pi_bridge.loaded_engine()
        except (ValueError, OSError, RuntimeError) as exc:
            raise Problems([Issue(code="pi_unavailable", node_id=agent.id, message=str(exc))]) from exc

    def start(self, revision: Revision, request: RunRequest, *, surface: RunSurface = "blueprints",
              tool_chat: bool = False) -> RunSummary:
        graph = graph_for(revision.blueprint)
        validate_request(graph, request)
        return self._start(revision, request, graph, surface=surface, tool_chat=tool_chat)

    def test_node(self, blueprint_id: str, revision: int, test: NodeTestRequest) -> RunSummary:
        node = test.node
        graph = node_test_graph(node)
        inputs = test.inputs
        if node.kind == "input":
            validate_ports(inputs, node.outputs, node.id, "inputs")
            request = RunRequest(mode="local", inputs={node.id: inputs["value"]})
            inputs = {}
        else:
            validate_ports(inputs, node.inputs, node.id, "inputs")
            request = RunRequest(mode="local")
        snapshot = Revision(id=blueprint_id, revision=revision, updated_at=now(),
                            blueprint=Blueprint(version=1, name=test.name, nodes=[node]))
        return self._start(snapshot, request, graph, surface="blueprints", test_inputs=inputs)

    def _start(self, revision, request, graph, *, surface, test_inputs=None, tool_chat=False) -> RunSummary:
        bindings = self.resolver.resolve(graph)
        tool_engine, tool_model = None, None
        if request.mode == "local":
            check_runtime(graph, bindings)
            tool_engine = self._tool_engine(graph)
            if any(isinstance(n, ToolNode) for n in graph.nodes.values()):
                try:
                    path = request.working_directory
                    if path is None:
                        default = Path(app_home()).absolute() / "tool-workspace"
                        default.mkdir(exist_ok=True)
                        path = str(default)
                    directory = resolve_directory(path)
                except (ValueError, OSError, RuntimeError) as exc:
                    raise Problems([Issue(code="invalid_working_directory", message=str(exc))]) from exc
                request = request.model_copy(update={"working_directory": str(directory)})
                if tool_engine is not None:
                    tool_model = {"engine": tool_engine.name, "model": tool_engine.model_ref}
        snapshot = RunSnapshot(blueprint=revision.blueprint, bindings=bindings, request=request,
                               tool_model=tool_model, test_inputs=test_inputs)
        run = RunSummary(id=uuid4().hex, blueprint_id=revision.id, revision=revision.revision,
                         name=revision.blueprint.name, mode=request.mode, surface=surface, created_at=now(),
                         test_node=graph.order[0] if test_inputs is not None else None, tool_chat=tool_chat)
        job = _Job(run, snapshot, graph, {node: Step(node_id=node) for node in graph.order},
                   tool_engine=tool_engine)
        with self._lock:
            if self._closing:
                raise RunnerUnavailable("The Blueprint runner is shutting down")
            if len(self._jobs) >= _MAX_ACTIVE:
                raise RunnerUnavailable("The Blueprint run queue is full")
            self.store.create_run(run, snapshot, list(job.steps.values()))
            self._jobs[run.id] = job
            self._executor.submit(self._work, job)
        return run

    def has_active(self) -> bool:
        with self._lock:
            return self._working or bool(self._jobs)

    def cancel(self, run_id: str) -> RunSummary | None:
        with self._lock:
            job = self._jobs.get(run_id)
            if job is None:
                return self.store.summary(run_id)
            job.stop.set()
            self._finish(job, "cancelled")
            return job.run

    def request_stop(self) -> None:
        with self._lock:
            self._closing = True
            for job in self._jobs.values():
                job.stop.set()

    def shutdown(self) -> None:
        self.request_stop()
        self._executor.shutdown(wait=True)

    def _can_continue(self, job: _Job) -> bool:
        if job.run.finished_at is not None:
            return False
        if job.stop.is_set():
            self._finish(job, "cancelled")
            return False
        return True

    def _set_step(self, job: _Job, step: Step) -> None:
        self.store.transition(job.run, [step])
        job.steps[step.node_id] = step

    def _finish(self, job: _Job, status: RunStatus, issue: Issue | None = None,
                *, last_step: Step | None = None, results_json: str | None = None) -> None:
        at = now()
        changed = [last_step] if last_step is not None else []
        remaining = (step for step in job.steps.values() if last_step is None or step.node_id != last_step.node_id)
        elapsed = (monotonic() - job.step_started) * 1000 if job.step_started is not None else None
        changed.extend(_terminal_steps(remaining, status, at, issue, elapsed))
        run = job.run.model_copy(update={"status": status, "finished_at": at, "error": issue})
        self.store.transition(run, changed, run_event=True, results_json=results_json)
        job.run = run
        del self._jobs[run.id]

    def _work(self, job: _Job) -> None:
        session = TorchBlueprintSession() if job.run.mode == "local" else None
        with self._lock:
            self._working = True
        try:
            self._run_graph(job, session)
        except Exception:
            logger.exception("Unexpected Blueprint run failure (%s)", job.run.id)
            with self._lock:
                if self._can_continue(job):
                    self._finish(job, "failed", Issue(code="internal_error", message="Blueprint executor failed"))
        finally:
            try:
                if session is not None:
                    session.close()
            finally:
                with self._lock:
                    self._working = False

    def _run_tool(self, node: ToolNode, value, job: _Job) -> dict:
        directory = Path(job.snapshot.request.working_directory)

        def approve(proposal):
            return self.broker.perform(job.run.id, node.id, proposal, directory, job.stop)

        if node.operation == "agent":
            try:
                return {"receipt": {"text": pi_bridge.run(value, job.tool_engine, approve, job.stop,
                                                          working_directory=str(directory))}}
            except (ValueError, OSError, RuntimeError) as exc:
                raise Problems([Issue(code="pi_failed", node_id=node.id, message=str(exc))]) from exc
        try:
            result = approve(value)
        except (ValueError, OSError, RuntimeError) as exc:
            raise Problems([Issue(code="invalid_tool_action", node_id=node.id,
                                  message="Invalid tool action or unavailable target")]) from exc
        if result["status"] != "completed":
            raise Problems([Issue(code="tool_" + result["status"], node_id=node.id,
                                  message="Tool action ended with status " + result["status"])])
        return {"receipt": {"decision": "approved", **result}}

    def _run_graph(self, job: _Job, session: TorchBlueprintSession | None) -> None:
        with self._lock:
            if not self._can_continue(job):
                return
            running = job.run.model_copy(update={"status": "running", "started_at": now()})
            self.store.transition(running, [], run_event=True)
            job.run = running
        results = {}
        trace_bytes = 0
        for index, node_id in enumerate(job.graph.order):
            node = job.graph.nodes[node_id]
            inputs = job.snapshot.test_inputs if job.run.test_node else {
                port: job.steps[source].outputs[source_port]
                for port, (source, source_port) in job.graph.sources[node_id].items()
                if port in node.inputs
            }
            with self._lock:
                if not self._can_continue(job):
                    return
                job.step_started = monotonic()
                step = job.steps[node_id].model_copy(update={"status": "running", "started_at": now()})
                self._set_step(job, step)
            issue = None
            try:
                trace_bytes = _trace_size(trace_bytes, inputs, node_id, "inputs")
                display_inputs = inputs if node.kind != "input" else {"value": job.snapshot.request.inputs[node.id]}
                step = step.model_copy(update={"inputs": display_inputs})
                validate_ports(inputs, node.inputs, node_id, "inputs")
                if isinstance(node, ToolNode) and job.run.mode == "local":
                    outputs = self._run_tool(node, inputs["value"], job)
                else:
                    outputs = _execute(node, inputs, job.snapshot, job.stop, session)
                if job.stop.is_set():
                    with self._lock:
                        self._can_continue(job)
                    return
                trace_bytes = _trace_size(trace_bytes, outputs, node_id, "outputs")
                step = step.model_copy(update={"outputs": outputs})
                validate_ports(outputs, node.outputs, node_id, "outputs")
                if isinstance(node, ToolNode) and node.operation == "approval" and job.run.mode == "fixture":
                    _validate_receipt(node_id, outputs["receipt"])
                if job.run.test_node:
                    results[node_id] = outputs
                elif node.kind == "output":
                    results[node_id] = outputs["value"]
                last = index + 1 == len(job.graph.order)
                results_json = _json(results) if last else None
            except Problems as exc:
                issue = exc.issues[0]
            except ValueError as exc:
                issue = Issue(code="operation_failed", node_id=node_id, message=str(exc))

            with self._lock:
                if not self._can_continue(job):
                    return
                elapsed = (monotonic() - job.step_started) * 1000
                step = _finished_step(step, "failed" if issue else "passed", now(), elapsed, issue)
                if issue:
                    self._finish(job, "failed", issue, last_step=step)
                    return
                if last:
                    self._finish(job, "passed", last_step=step, results_json=results_json)
                    return
                self._set_step(job, step)
                job.step_started = None


def _trace_size(used: int, values, node_id: str, path: str) -> int:
    size = used + len(_json(values).encode("utf-8"))
    if size > _MAX_TRACE_BYTES:
        raise Problems([Issue(code="trace_limit", node_id=node_id, path=path,
                              message="Run inputs and outputs exceed the 32 MiB trace limit")])
    return size


def _validate_receipt(node_id: str, receipt: dict) -> None:
    path = "outputs.receipt.decision"
    decision = receipt.get("decision")
    if decision == "denied":
        raise Problems([Issue(code="tool_denied", node_id=node_id, path=path,
                              message="Fixture tool request was denied")])
    if decision != "approved":
        raise Problems([Issue(code="invalid_value", node_id=node_id, path=path,
                              message="Expected a recorded 'approved' or 'denied' fixture decision")])
