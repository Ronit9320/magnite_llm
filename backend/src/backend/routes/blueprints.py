from __future__ import annotations

import asyncio
import json
import os
from pathlib import Path
import re
from typing import Annotated

from fastapi import APIRouter, Header, HTTPException, Query, Request, Response
from fastapi.concurrency import run_in_threadpool
from fastapi.exceptions import RequestValidationError
from fastapi.responses import StreamingResponse
from fastapi.routing import APIRoute

from backend.blueprints.bindings import BindingResolver
from backend.blueprints.graph import graph_for
from backend.blueprints.runner import BlueprintRunner, RunnerUnavailable
from backend.blueprints.runtime import check_adapter, read_checkpoint
from backend.blueprints.schemas import (
    SQLITE_MAX_INTEGER, Blueprint, BlueprintSummary, GenerationNode, NodeId, NodeTestRequest, Problems, RegistryId, Revision, Run, RunRequest,
    RunSummary, RunSurface, ToolNode, ValidationResult,
)
from backend.blueprints.store import BlueprintStore, RevisionConflict
from backend.blueprints.training import NodeAdapterRequest, NodeTrainingRequest, start_node_training
from backend.paths import app_home
from backend.routes.adapters import _get_store
from backend.routes.models import _get_registry
from backend.routes.training import _get_datasets, _get_manager
from backend.training.schemas import TrainingJob


class _BlueprintRoute(APIRoute):
    def get_route_handler(self):
        handler = super().get_route_handler()

        async def handle(request):
            try:
                return await handler(request)
            except RequestValidationError as exc:
                # FastAPI normally echoes rejected input verbatim. Keep error locations
                # without emitting invalid Unicode or non-finite numbers from the body.
                errors = [{key: item[key] for key in ("loc", "msg", "type")} for item in exc.errors()]
                return Response(json.dumps({"detail": errors}), status_code=422,
                                media_type="application/json")

        return handle


router = APIRouter(prefix="/blueprints", tags=["blueprints"], route_class=_BlueprintRoute)
_runner: BlueprintRunner | None = None

RevisionQuery = Annotated[int | None, Query(ge=1, le=SQLITE_MAX_INTEGER)]
Limit = Annotated[int, Query(ge=1, le=100)]
Offset = Annotated[int, Query(ge=0, le=SQLITE_MAX_INTEGER)]


def blueprint_setup() -> None:
    global _runner
    store = BlueprintStore(os.path.join(app_home(), "blueprints.db"))
    _runner = BlueprintRunner(store, BindingResolver(_get_registry(), _get_store(), _get_datasets()))


def blueprint_runner() -> BlueprintRunner:
    if _runner is None:
        raise HTTPException(503, detail="Blueprint runner is unavailable")
    return _runner


def blueprint_busy() -> bool:
    return _runner is not None and _runner.has_active()


def stop_blueprint_runs() -> None:
    if _runner is not None:
        _runner.request_stop()


def shutdown_blueprints() -> None:
    global _runner
    if _runner is not None:
        _runner.shutdown()
        _runner = None


def _revision(blueprint_id: str, revision: int | None = None) -> Revision:
    saved = blueprint_runner().store.revision(blueprint_id, revision)
    if saved is None:
        raise HTTPException(404, detail="Blueprint revision not found")
    return saved


def _invalid(exc: Problems) -> HTTPException:
    return HTTPException(422, detail=[issue.model_dump(exclude_none=True) for issue in exc.issues])


def require_tool_auth(request: Request) -> None:
    if not request.scope.get("magnite_authenticated"):
        raise HTTPException(403, detail="Tool runs require an authenticated Magnite backend launch")


def _generation_node(revision: Revision, node_id: str) -> GenerationNode:
    for node in revision.blueprint.nodes:
        if node.id == node_id and isinstance(node, GenerationNode):
            return node
    raise HTTPException(404, detail="Model or Reasoning node not found")


@router.get("", response_model=list[BlueprintSummary])
def list_blueprints(limit: Limit = 50, offset: Offset = 0):
    return blueprint_runner().store.revisions(limit, offset)


@router.post("", response_model=Revision, status_code=201, response_model_exclude_none=True)
def create_blueprint(body: Blueprint):
    try:
        graph_for(body)
    except Problems as exc:
        raise _invalid(exc) from exc
    return blueprint_runner().store.save(body)


@router.post("/validate", response_model=ValidationResult, response_model_exclude_none=True)
def validate_blueprint(body: Blueprint):
    try:
        blueprint_runner().validate(body)
    except Problems as exc:
        return ValidationResult(valid=False, issues=exc.issues)
    return ValidationResult(valid=True, issues=[])


@router.post("/capabilities")
def blueprint_capabilities(body: Blueprint):
    try:
        return blueprint_runner().capabilities(body)
    except Problems as exc:
        raise _invalid(exc) from exc


@router.get("/checkpoints")
def blueprint_checkpoints():
    resolver = blueprint_runner().resolver
    models = []
    records = resolver.models.list_synced(source="huggingface") + resolver.models.list_synced(source="local")
    for record in records:
        try:
            resolver.model(record.id)
        except (ValueError, OSError):
            continue
        models.append({"id": record.id, "name": record.name, "thinking": _template_thinks(record.path)})
    return models


def _template_thinks(path: str) -> bool:
    """Cheap check for a chat template with a thinking switch; the detail route confirms it."""
    root = Path(path)
    for name in ("chat_template.jinja", "tokenizer_config.json"):
        try:
            text = (root / name).read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if "enable_thinking" in text or "</think>" in text:
            return True
    return False


@router.get("/checkpoints/{model_id}")
def blueprint_checkpoint(model_id: RegistryId):
    resolver = blueprint_runner().resolver
    try:
        model = resolver.model(model_id)
        checkpoint = read_checkpoint(model)
    except (ValueError, OSError, ImportError, RuntimeError) as exc:
        raise HTTPException(422, detail=str(exc)) from exc
    compatible = []
    for record in resolver.adapters.list():
        try:
            adapter = resolver.adapter(record.id)
            resolver.check_compatible(adapter, model)
            check_adapter(checkpoint, adapter)
        except (ValueError, OSError, ImportError):
            continue
        compatible.append({"id": record.id, "name": record.name})
    return {"adapters": compatible, "reasoning": checkpoint.reasoning, "context_tokens": checkpoint.context}


@router.post("/import", response_model=Revision, status_code=201, response_model_exclude_none=True)
def import_blueprint(body: Blueprint):
    runner = blueprint_runner()
    try:
        runner.validate(body)
    except Problems as exc:
        raise _invalid(exc) from exc
    return runner.store.save(body)


@router.get("/runs", response_model=list[RunSummary])
def list_runs(blueprint_id: str | None = None, limit: Limit = 50, offset: Offset = 0,
              surface: RunSurface | None = None, active: bool = False):
    return blueprint_runner().store.history(blueprint_id, limit, offset, surface, active)


@router.get("/runs/{run_id}", response_model=Run, response_model_exclude_none=True)
def get_run(run_id: str):
    run = blueprint_runner().store.run(run_id)
    if run is None:
        raise HTTPException(404, detail="Blueprint run not found")
    return run


@router.post("/runs/{run_id}/cancel", response_model=RunSummary)
def cancel_run(run_id: str):
    run = blueprint_runner().cancel(run_id)
    if run is None:
        raise HTTPException(404, detail="Blueprint run not found")
    return run


@router.get("/runs/{run_id}/events", response_class=StreamingResponse)
async def run_events(run_id: str, after: Offset = 0,
                     last_event_id: Annotated[str | None, Header()] = None):
    if last_event_id:
        if not re.fullmatch(r"[0-9]{1,19}", last_event_id) or int(last_event_id) > SQLITE_MAX_INTEGER:
            raise HTTPException(422, detail="Invalid Last-Event-ID")
        after = max(after, int(last_event_id))

    store = blueprint_runner().store
    if await run_in_threadpool(store.summary, run_id) is None:
        raise HTTPException(404, detail="Blueprint run not found")

    async def stream():
        cursor = after
        loop = asyncio.get_running_loop()
        keepalive_at = loop.time() + 15
        while True:
            events, terminal = await run_in_threadpool(store.events, run_id, cursor)
            for event in events:
                cursor = event["sequence"]
                yield f"id: {cursor}\nevent: {event['kind']}\ndata: {event['payload']}\n\n"
            if terminal:
                return
            if events or keepalive_at <= loop.time():
                if not events:
                    yield ": keep-alive\n\n"
                keepalive_at = loop.time() + 15
            await asyncio.sleep(0.2)

    return StreamingResponse(stream(), media_type="text/event-stream", headers={"Cache-Control": "no-cache"})


@router.get("/{blueprint_id}", response_model=Revision, response_model_exclude_none=True)
def get_blueprint(blueprint_id: str, revision: RevisionQuery = None):
    return _revision(blueprint_id, revision)


@router.put("/{blueprint_id}", response_model=Revision, response_model_exclude_none=True)
def update_blueprint(blueprint_id: NodeId, body: Blueprint,
                     expected_revision: Annotated[int | None, Query(ge=0, le=SQLITE_MAX_INTEGER)] = None):
    try:
        graph_for(body)
    except Problems as exc:
        raise _invalid(exc) from exc
    try:
        if expected_revision == 0 and not re.fullmatch(r"[a-f0-9]{32}", blueprint_id):
            raise HTTPException(422, detail="A new blueprint ID must contain 32 hexadecimal characters")
        saved = blueprint_runner().store.save(body, blueprint_id, expected_revision=expected_revision)
    except RevisionConflict as exc:
        raise HTTPException(409, detail=str(exc)) from exc
    if saved is None:
        raise HTTPException(404, detail="Blueprint not found")
    return saved


@router.delete("/{blueprint_id}", status_code=204)
def delete_blueprint(blueprint_id: NodeId, expected_revision: Annotated[int, Query(ge=1, le=SQLITE_MAX_INTEGER)]):
    try:
        if not blueprint_runner().store.delete(blueprint_id, expected_revision):
            raise HTTPException(404, detail="Blueprint not found")
    except RevisionConflict as exc:
        raise HTTPException(409, detail=str(exc)) from exc
    return Response(status_code=204)


@router.get("/{blueprint_id}/export", response_model=Blueprint, response_model_exclude_none=True)
def export_blueprint(blueprint_id: str, revision: RevisionQuery = None):
    return _revision(blueprint_id, revision).blueprint


@router.post("/{blueprint_id}/runs", response_model=RunSummary, status_code=202)
def start_run(blueprint_id: str, body: RunRequest, request: Request, surface: RunSurface = "blueprints"):
    revision = _revision(blueprint_id, body.revision)
    if body.mode == "local" and any(isinstance(node, ToolNode) for node in revision.blueprint.nodes):
        require_tool_auth(request)
    try:
        return blueprint_runner().start(revision, body, surface=surface)
    except Problems as exc:
        raise _invalid(exc) from exc
    except RunnerUnavailable as exc:
        raise HTTPException(503, detail=str(exc)) from exc


@router.post("/{blueprint_id}/node-tests", response_model=RunSummary, status_code=202)
def test_blueprint_node(blueprint_id: NodeId, body: NodeTestRequest, request: Request):
    if isinstance(body.node, ToolNode):
        require_tool_auth(request)
    runner = blueprint_runner()
    saved = runner.store.revision(blueprint_id)
    revision = (saved.revision if saved and saved.blueprint.name == body.name
                and body.node in saved.blueprint.nodes else 0)
    try:
        return runner.test_node(blueprint_id, revision, body)
    except Problems as exc:
        raise _invalid(exc) from exc
    except RunnerUnavailable as exc:
        raise HTTPException(503, detail=str(exc)) from exc


@router.post("/{blueprint_id}/nodes/{node_id}/training", response_model=TrainingJob, status_code=202)
def train_node(blueprint_id: str, node_id: str, body: NodeTrainingRequest):
    revision = _revision(blueprint_id, body.revision)
    node = _generation_node(revision, node_id)
    try:
        return start_node_training(blueprint_runner().resolver, _get_manager(), revision, node, body)
    except Problems as exc:
        raise _invalid(exc) from exc
    except (ValueError, OSError, ImportError) as exc:
        raise HTTPException(422, detail=str(exc)) from exc
    except RuntimeError as exc:
        raise HTTPException(503, detail=str(exc)) from exc


@router.put("/{blueprint_id}/nodes/{node_id}/adapter", response_model=Revision, response_model_exclude_none=True)
def bind_node_adapter(blueprint_id: str, node_id: str, body: NodeAdapterRequest):
    revision = _revision(blueprint_id)
    if revision.revision != body.revision:
        raise HTTPException(409, detail="The blueprint was edited. Reload its latest revision and retry.")
    node = _generation_node(revision, node_id)
    if "adapter" not in node.inputs and any(
        edge.target.node == node_id and edge.target.port == "adapter" for edge in revision.blueprint.edges
    ):
        raise HTTPException(409, detail="Select the adapter on the connected Adapter node")
    adapter_id = body.adapter_id
    if body.training_job_id:
        job = _get_manager().get(body.training_job_id)
        if job is None:
            raise HTTPException(404, detail="Training job not found")
        origin = job.config.get("blueprint_node") or {}
        if origin.get("blueprint_id") != blueprint_id or origin.get("node_id") != node_id:
            raise HTTPException(422, detail="Training job belongs to a different Blueprint node")
        if job.status != "done" or not job.adapter_id:
            raise HTTPException(409, detail="Training job has no completed adapter")
        original = _generation_node(_revision(blueprint_id, origin["revision"]), node_id)
        contract = ("kind", "inputs", "outputs", "response", "system_prompt")
        if any(getattr(original, field) != getattr(node, field) for field in contract):
            raise HTTPException(409, detail="Node contract changed after training was queued")
        adapter_id = job.adapter_id

    runner = blueprint_runner()
    try:
        model = runner.resolver.model(node.binding.model_id)
        adapter = runner.resolver.adapter(adapter_id)
        runner.resolver.check_compatible(adapter, model)
        check_adapter(read_checkpoint(model), adapter)
        node.binding.adapter_id = adapter_id
        return runner.store.save(revision.blueprint, blueprint_id, expected_revision=body.revision)
    except RevisionConflict as exc:
        raise HTTPException(409, detail=str(exc)) from exc
    except (ValueError, OSError, ImportError) as exc:
        raise HTTPException(422, detail=str(exc)) from exc
