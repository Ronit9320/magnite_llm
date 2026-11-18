"""Offline coverage of the authenticated Blueprint contract and durable run lifecycle."""
from contextlib import ExitStack
from copy import deepcopy
import json
import os
from pathlib import Path
import tempfile
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch
from uuid import uuid4

from fastapi.testclient import TestClient
import numpy as np
from safetensors.numpy import save_file

from backend.adapters.schemas import AdapterRecord
from backend.blueprints.schemas import Bindings, ModelSnapshot, RunRequest, RunSnapshot, RunSummary, Step
from backend.blueprints.store import BlueprintStore, now
from backend.database import connection
from backend.local_access import LocalAccessMiddleware
from backend.models.schemas import ModelRecord
from backend.routes.adapters import _get_store
from backend.routes.models import _get_registry
from backend.routes.training import _get_datasets, _get_manager
from backend.server import app
from backend.training.schemas import TrainingJob
from order_blueprint_fixture import order_blueprint, order_samples, training_examples

STRING = {"type": "string"}
INTEGER = {"type": "integer"}
OBJECT = {"type": "object"}
RESULT = {"type": "object", "properties": {"quantity": INTEGER, "note": {"type": "null"}},
          "required": ["quantity", "note"], "additionalProperties": False}
TOKEN = "blueprint-test-token"
MODEL_ID = "huggingface:local-test--checkpoint"
WORKFLOW_EXAMPLES = Path(__file__).resolve().parents[2] / "docs/development/blueprints/examples"


def node(node_id, kind, operation, *, inputs=None, outputs=None, **fields):
    return {"id": node_id, "kind": kind, "operation": operation,
            "inputs": inputs or {}, "outputs": outputs or {}, **fields}


def edge(source, target, out_port="value", in_port="value"):
    return {"source": {"node": source, "port": out_port}, "target": {"node": target, "port": in_port}}


def blueprint(*nodes, edges=(), name="Order count"):
    return {"version": 1, "name": name, "nodes": list(nodes), "edges": list(edges)}


def source(schema=STRING):
    return node("request", "input", "input", outputs={"value": schema})


def output(schema=STRING):
    return node("result", "output", "output", inputs={"value": schema}, outputs={"value": schema})


def simple_graph():
    return blueprint(source(), output(), edges=[edge("request", "result")])


def model_graph(reasoning=False, adapter=None):
    kind, operation = ("reasoning", "reason") if reasoning else ("model", "generate")
    binding = {"model_id": MODEL_ID}
    if adapter:
        binding["adapter_id"] = adapter
    return blueprint(
        output(RESULT), node("generate", kind, operation, inputs={"prompt": STRING},
                             outputs={"order": RESULT}, binding=binding), source(),
        edges=[edge("generate", "result", "order"), edge("request", "generate", in_port="prompt")])


def model_request(quantity=7, delay=0):
    return {"inputs": {"request": "seven items"}, "fixtures": {
        "generate": {"outputs": {"order": {"quantity": quantity, "note": None}}, "delay_ms": delay}
    }}


def read_events(body):
    return [
        (int(lines[0].removeprefix("id: ")), lines[1].removeprefix("event: "),
         json.loads(lines[2].removeprefix("data: ")))
        for block in body.strip().split("\n\n") if block.startswith("id: ")
        for lines in [block.splitlines()]
    ]


class BlueprintApiTests(unittest.TestCase):
    def setUp(self):
        resources = ExitStack()
        self.addCleanup(resources.close)
        self.home = Path(resources.enter_context(tempfile.TemporaryDirectory()))
        resources.enter_context(patch.dict(os.environ, {"MAGNITE_HOME": str(self.home)}))
        resources.enter_context(patch("backend.runtime_packages.activate"))
        self.session = ExitStack()
        self.addCleanup(self.session.close)
        self.open_client()

    def open_client(self):
        self.client = self.session.enter_context(
            TestClient(LocalAccessMiddleware(app, TOKEN), base_url="http://127.0.0.1",
                       headers={"Authorization": f"Bearer {TOKEN}"}))

    def close_client(self):
        self.session.close()

    def save(self, graph):
        response = self.client.post("/blueprints", json=graph)
        self.assertEqual(response.status_code, 201, response.text)
        return response.json()

    def start(self, saved, request):
        response = self.client.post(f"/blueprints/{saved['id']}/runs", json=request)
        self.assertEqual(response.status_code, 202, response.text)
        self.assertEqual(response.json()["status"], "queued")
        return f"/blueprints/runs/{response.json()['id']}"

    def terminal(self, path):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            result = self.client.get(path).json()
            if result["status"] not in ("queued", "running"):
                break
            time.sleep(0.01)
        else:
            self.fail(f"Run did not finish: {result}")
        response = self.client.get(f"{path}/events")
        self.assertEqual(response.status_code, 200, response.text)
        events = read_events(response.text)
        self.assertEqual(events[-1][2]["status"], result["status"])
        return result, events

    def wait_for_node(self, path, node_id):
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            run = self.client.get(path).json()
            if any(s["node_id"] == node_id and s["status"] == "running" for s in run["steps"]):
                return
            time.sleep(0.01)
        self.fail(f"{node_id} never started: {run}")

    def register_model(self):
        weights = self.home / "model-snapshot"
        weights.mkdir()
        (weights / "config.json").write_text("{}", encoding="utf-8")
        (weights / "model.safetensors").write_bytes(b"fixture")
        _get_registry()._store.upsert(ModelRecord(id=MODEL_ID, name="Test checkpoint", source="huggingface",
                                                  path=str(weights), ref="local-test/checkpoint"))
        return weights

    def register_adapter(self, base_model, *, adapter_id="adapter-1", **meta):
        weights = self.home / adapter_id
        weights.mkdir()
        (weights / "adapter_config.json").write_text(
            '{"peft_type":"LORA","r":1,"lora_alpha":2,"target_modules":["q_proj"]}', encoding="utf-8")
        save_file({"test": np.zeros(1, dtype=np.float32)}, str(weights / "adapter_model.safetensors"))
        _get_store().upsert(AdapterRecord(id=adapter_id, name="Test adapter", path=str(weights),
                                         base_model=base_model, training_job_id="training-fixture", meta=meta))

    def test_portable_round_trip_versioned_history_and_auth(self):
        self.assertEqual(self.client.get("/blueprints", headers={"Authorization": ""}).status_code, 401)
        source_id = "1f84b5c2-402"
        graph = simple_graph()
        graph["nodes"][0]["id"] = source_id
        graph["edges"][0]["source"]["node"] = source_id
        original = self.save(graph)
        path = f"/blueprints/{original['id']}"
        export = self.client.get(f"{path}/export").json()
        imported = self.client.post("/blueprints/import", json=export)
        self.assertEqual(imported.status_code, 201, imported.text)
        self.assertNotEqual(imported.json()["id"], original["id"])
        self.assertEqual(imported.json()["blueprint"], export)

        export["name"] = "Revision two"
        self.assertEqual(self.client.put(path, json=export).json()["revision"], 2)
        self.assertEqual(self.client.get(path).json()["blueprint"]["name"], "Revision two")
        self.assertEqual(self.client.get(path, params={"revision": 1}).json(), original)
        self.assertEqual(self.client.get("/blueprints?limit=1").json()[0]["revision"], 2)

        run_path = self.start(original, {"revision": 1, "inputs": {source_id: "hello"}})
        result, events = self.terminal(run_path)
        self.assertEqual(result["status"], "passed")
        self.assertEqual(result["mode"], "fixture")
        self.assertEqual(result["revision"], 1)
        self.assertEqual(result["results"], {"result": "hello"})
        self.assertEqual(result["snapshot"]["blueprint"], original["blueprint"])
        self.assertEqual([payload["status"] for _, kind, payload in events if kind == "run"],
                         ["queued", "running", "passed"])
        self.assertTrue(all(step["duration_ms"] >= 0 for step in result["steps"]))

        self.close_client()
        self.open_client()
        self.assertEqual(self.client.get(run_path).json(), result)
        suffix = self.client.get(f"{run_path}/events", headers={"Last-Event-ID": str(events[1][0])})
        self.assertEqual(read_events(suffix.text), events[2:])
        self.assertEqual(self.client.get(f"{run_path}/events", params={"after": events[-1][0]}).text, "")
        self.assertEqual(self.client.get("/blueprints/runs", params={"blueprint_id": original["id"]})
                         .json()[0]["id"], result["id"])
        self.assertEqual(self.client.get("/blueprints/runs", params={"blueprint_id": "none"}).json(), [])

    def test_desktop_publish_retry_conflict_and_delete_keep_run_snapshot(self):
        identifier = uuid4().hex
        path = f"/blueprints/{identifier}"
        original = simple_graph()
        create = {"expected_revision": 0}
        saved = self.client.put(path, params=create, json=original)
        self.assertEqual(saved.status_code, 200, saved.text)
        self.assertEqual(self.client.put(path, params=create, json=original).json(), saved.json())
        edited = {**original, "name": "Edited graph"}
        self.assertEqual(self.client.put(path, params=create, json=edited).status_code, 409)
        self.assertEqual(self.client.put(path, params={"expected_revision": 1}, json=edited).json()["revision"], 2)
        self.assertEqual(self.client.delete(path, params={"expected_revision": 1}).status_code, 409)

        result = self.client.post(f"{path}/runs?surface=chat", json={
            "revision": 1, "mode": "local", "inputs": {"request": "Pinned revision"},
        })
        self.assertEqual(result.status_code, 202, result.text)
        run_path = f"/blueprints/runs/{result.json()['id']}"
        run, _ = self.terminal(run_path)
        self.assertEqual(run["results"], {"result": "Pinned revision"})
        self.assertEqual(run["surface"], "chat")
        self.assertEqual(self.client.get("/blueprints/runs?surface=chat").json()[0]["revision"], 1)
        self.assertEqual(self.client.delete(path, params={"expected_revision": 2}).status_code, 204)
        self.assertEqual(self.client.get(path).status_code, 404)
        self.assertEqual(self.client.get(run_path).json(), run)

    def test_isolated_node_test_persists_inputs_and_uses_the_selected_contract(self):
        identifier = uuid4().hex
        path = f"/blueprints/{identifier}/node-tests"
        count = node("count", "transform", "count", inputs={"value": {"type": "array"}},
                     outputs={"value": INTEGER})
        test = {"name": "Draft dataset", "node": count, "inputs": {"value": [1, 2, 3]}}
        result = self.client.post(path, json=test)
        self.assertEqual(result.status_code, 202, result.text)
        run_path = f"/blueprints/runs/{result.json()['id']}"
        run, _ = self.terminal(run_path)
        self.assertEqual((run["revision"], run["test_node"]), (0, "count"))
        self.assertEqual(run["snapshot"]["test_inputs"], test["inputs"])
        self.assertEqual(run["results"], {"count": {"value": 3}})
        self.assertEqual([(step["node_id"], step["inputs"]) for step in run["steps"]],
                         [("count", {"value": [1, 2, 3]})])
        bad = self.client.post(path, json={**test, "inputs": {"value": "not an array"}})
        self.assertEqual(bad.status_code, 422, bad.text)
        self.assertEqual(bad.json()["detail"][0]["path"], "inputs.value")
        self.assertEqual(len(self.client.get(f"/blueprints/runs?blueprint_id={identifier}").json()), 1)

    def test_graph_structure_and_schema_errors(self):
        wrong_type = blueprint(source(), output(INTEGER), edges=[edge("request", "result")])
        nested = blueprint(source({"type": "array", "items": STRING}),
                           output({"type": "array", "items": INTEGER}), edges=[edge("request", "result")])
        duplicate = simple_graph()
        duplicate["edges"].append(duplicate["edges"][0])
        missing = simple_graph()
        missing["edges"] = [edge("request", "result", out_port="missing")]
        a = node("parseA", "transform", "pass", inputs={"value": STRING}, outputs={"value": STRING})
        b = deepcopy(a)
        b["id"] = "parseB"
        cycle = blueprint(a, b, output(), edges=[edge("parseA", "parseB"), edge("parseB", "parseA"),
                                                  edge("parseB", "result")])
        for graph, code in [(wrong_type, "incompatible_ports"), (nested, "incompatible_ports"),
                            (duplicate, "duplicate_connection"), (missing, "unknown_port"), (cycle, "cycle")]:
            with self.subTest(code=code, graph=graph):
                validation = self.client.post("/blueprints/validate", json=graph).json()
                self.assertFalse(validation["valid"])
                self.assertIn(code, [issue["code"] for issue in validation["issues"]])
                self.assertEqual(self.client.post("/blueprints", json=graph).status_code, 422)

        body = simple_graph()
        for version in (2, True, 1.0):
            self.assertEqual(self.client.post("/blueprints/import", json=dict(body, version=version))
                             .status_code, 422)
        self.assertEqual(self.client.post("/blueprints/import", json=dict(body, script="echo unsafe"))
                         .status_code, 422)
        body["nodes"][0]["outputs"]["value"] = {"type": "string", "minimum": 0}
        self.assertEqual(self.client.post("/blueprints", json=body).status_code, 422)

    def test_bindings_are_checked_at_validation_import_and_start(self):
        graph = model_graph(reasoning=True, adapter="adapter-1")
        saved = self.save(graph)
        start_path = f"/blueprints/{saved['id']}/runs"
        self.assertEqual(self.client.post("/blueprints/validate", json=graph).json()["issues"][0]["path"],
                         "binding.model_id")
        self.assertEqual(self.client.post("/blueprints/import", json=graph).status_code, 422)
        self.assertEqual(self.client.post(start_path, json=model_request()).status_code, 422)
        self.assertEqual(self.client.get("/blueprints/runs").json(), [])

        weights = self.register_model()
        self.register_adapter(MODEL_ID, resolved_base_model=str(self.home / "old-model-snapshot"))
        issue = self.client.post(start_path, json=model_request()).json()["detail"][0]
        self.assertEqual((issue["node_id"], issue["path"]), ("generate", "binding.adapter_id"))
        record = _get_store().get("adapter-1")
        record.meta["resolved_base_model"] = str(weights)
        _get_store().upsert(record)
        self.assertTrue(self.client.post("/blueprints/validate", json=graph).json()["valid"])
        result, _ = self.terminal(self.start(saved, model_request()))
        self.assertEqual(result["results"], {"result": {"quantity": 7, "note": None}})
        bindings = result["snapshot"]["bindings"]
        self.assertEqual(bindings["models"]["generate"]["path"], str(weights.resolve()))
        self.assertEqual(bindings["adapters"]["generate"]["training_job_id"], "training-fixture")
        self.assertEqual(self.client.post("/blueprints/import", json=graph).status_code, 201)
        Path(record.path, "adapter_model.safetensors").unlink()
        self.assertEqual(self.client.post(start_path, json=model_request()).status_code, 422)
        self.assertEqual(self.client.get(f"/blueprints/runs/{result['id']}").json(), result)

    def test_wrong_model_output_fails_at_its_port_and_preserves_trace(self):
        self.register_model()
        path = self.start(self.save(model_graph()), model_request(quantity=True))
        result, _ = self.terminal(path)
        self.assertEqual(result["status"], "failed")
        self.assertEqual([(s["node_id"], s["status"]) for s in result["steps"]],
                         [("request", "passed"), ("generate", "failed"), ("result", "skipped")])
        error = result["error"]
        self.assertEqual((error["node_id"], error["path"]), ("generate", "outputs.order.quantity"))
        self.assertEqual(error, result["steps"][1]["error"])
        self.assertIs(result["steps"][1]["outputs"]["order"]["quantity"], True)
        self.assertEqual(result["results"], {})

    def test_linked_lora_is_resolved_without_becoming_a_model_prompt(self):
        self.register_model()
        self.register_adapter(MODEL_ID)
        graph = model_graph()
        graph["nodes"].append(node("lora", "adapter", "lora", outputs={"adapter": STRING}, adapter_id="adapter-1"))
        graph["edges"].append(edge("lora", "generate", "adapter", "adapter"))
        saved = self.save(graph)
        result, _ = self.terminal(self.start(saved, model_request()))
        self.assertEqual(result["status"], "passed")
        bindings = result["snapshot"]["bindings"]["adapters"]
        self.assertEqual(bindings["lora"], bindings["generate"])
        steps = {step["node_id"]: step for step in result["steps"]}
        self.assertEqual(steps["lora"]["outputs"], {"adapter": "adapter-1"})
        self.assertEqual(steps["generate"]["inputs"], {"prompt": "seven items"})

        conflict = deepcopy(graph)
        conflict["nodes"][1]["binding"]["adapter_id"] = "adapter-1"
        report = self.client.post("/blueprints/validate", json=conflict).json()
        self.assertEqual(report["issues"][0]["code"], "conflicting_adapter")
        wrong_port = deepcopy(graph)
        wrong_port["edges"][-1]["target"]["port"] = "prompt"
        report = self.client.post("/blueprints/validate", json=wrong_port).json()
        self.assertEqual(report["issues"][0]["code"], "unknown_port")
        binding = self.client.put(f"/blueprints/{saved['id']}/nodes/generate/adapter",
                                  json={"revision": 1, "adapter_id": "adapter-1"})
        self.assertEqual(binding.status_code, 409, binding.text)

    def test_support_triage_example_and_invalid_json_handoff(self):
        self.register_model()
        self.register_adapter(MODEL_ID, adapter_id="extractor-adapter")
        self.register_adapter(MODEL_ID, adapter_id="router-adapter")
        document = json.loads((WORKFLOW_EXAMPLES / "support-triage.json").read_text(encoding="utf-8"))
        for model in (n for n in document["nodes"] if n["kind"] == "model"):
            model["binding"]["model_id"] = MODEL_ID
        imported = self.client.post("/blueprints/import", json=document)
        self.assertEqual(imported.status_code, 201, imported.text)
        saved = imported.json()
        cases = json.loads((WORKFLOW_EXAMPLES / "support-triage-cases.json").read_text(encoding="utf-8"))
        for case in cases:
            with self.subTest(case=case["id"]):
                expected = case["expected"]
                request = {"mode": "fixture", "inputs": {"ticket": case["ticket"]}, "fixtures": {
                    "extract": {"outputs": {"raw_json": json.dumps(expected["ticket"])}},
                    "route": {"outputs": {"queue": expected["queue"], "priority": expected["priority"]}},
                }}
                run, _ = self.terminal(self.start(saved, request))
                self.assertEqual(run["status"], "passed", run.get("error"))
                self.assertEqual(run["results"], {"result": expected})
                steps = {step["node_id"]: step for step in run["steps"]}
                self.assertEqual(steps["extract"]["inputs"], {"ticket": case["ticket"]})
                self.assertEqual(steps["parse"]["outputs"], {"value": expected["ticket"]})
                self.assertEqual(steps["route"]["inputs"], {"record": expected["ticket"]})
                adapters = run["snapshot"]["bindings"]["adapters"]
                self.assertEqual(adapters["extract"]["id"], "extractor-adapter")
                self.assertEqual(adapters["route"]["id"], "router-adapter")

        for raw_json, code, path in [
            ('{"category":', "operation_failed", None),
            ('{"category":"billing","blocked":"true","reference":""}', "invalid_value", "outputs.value.blocked"),
            ('{"category":"billing","blocked":true}', "invalid_value", "outputs.value.reference"),
        ]:
            with self.subTest(raw_json=raw_json):
                invalid = {"mode": "fixture", "inputs": {"ticket": cases[0]["ticket"]}, "fixtures": {
                    "extract": {"outputs": {"raw_json": raw_json}},
                    "route": {"outputs": {"queue": "finance", "priority": "normal"}},
                }}
                run, _ = self.terminal(self.start(saved, invalid))
                self.assertEqual(run["status"], "failed")
                error = run["error"]
                self.assertEqual((error["node_id"], error["code"], error.get("path")), ("parse", code, path))
                steps = {step["node_id"]: step["status"] for step in run["steps"]}
                self.assertEqual([steps[node] for node in ("extract", "parse", "route", "pack", "result")],
                                 ["passed", "failed", "skipped", "skipped", "skipped"])
                self.assertEqual(run["results"], {})

    def test_retired_pi_agent_node_parses_but_is_rejected_with_a_repair_message(self):
        agent = node("triage", "agent", "pi-reply", inputs={"task": STRING}, outputs={"reply": STRING},
                     instructions="Summarize the user's request.")
        graph = blueprint(source(), agent, output(), edges=[
            edge("request", "triage", in_port="task"), edge("triage", "result", "reply")])
        validation = self.client.post("/blueprints/validate", json=graph).json()
        self.assertFalse(validation["valid"])
        issue = next(issue for issue in validation["issues"] if issue["code"] == "retired_node")
        self.assertEqual(issue["node_id"], "triage")
        self.assertIn("Model node", issue["message"])
        self.assertEqual(self.client.post("/blueprints", json=graph).status_code, 422)

    def test_checkpoint_list_reports_thinking_templates(self):
        from backend.routes.blueprints import _template_thinks

        with tempfile.TemporaryDirectory() as folder:
            root = Path(folder)
            self.assertFalse(_template_thinks(folder))
            (root / "tokenizer_config.json").write_text('{"chat_template": "{{ messages }}"}')
            self.assertFalse(_template_thinks(folder))
            (root / "chat_template.jinja").write_text("{% if enable_thinking %}<think>{% endif %}")
            self.assertTrue(_template_thinks(folder))

    def test_template_compare_choose_and_validate_route_a_typed_record(self):
        """Invoice review without a second generation: validate, compare, choose, and template."""
        number = {"type": "number"}
        invoice = {"type": "object", "properties": {"id": STRING, "amount": number, "po": STRING},
                   "required": ["id", "amount", "po"]}
        nodes = [
            source(OBJECT),
            node("check", "transform", "validate", inputs={"value": OBJECT}, outputs={"value": invoice},
                 parameters={"schema": invoice}),
            node("amount", "transform", "field", inputs={"value": invoice}, outputs={"value": number},
                 parameters={"field": "amount"}),
            node("limit", "transform", "constant", outputs={"value": number}, parameters={"value": 1000}),
            node("large", "transform", "compare", inputs={"left": number, "right": number},
                 outputs={"value": {"type": "boolean"}}, parameters={"operator": "greater"}),
            node("review", "transform", "constant", outputs={"value": STRING}, parameters={"value": "Needs review"}),
            node("auto", "transform", "constant", outputs={"value": STRING}, parameters={"value": "Approved"}),
            node("status", "transform", "choose",
                 inputs={"condition": {"type": "boolean"}, "if_true": STRING, "if_false": STRING},
                 outputs={"value": STRING}),
            node("note", "transform", "template", inputs={"invoice": invoice, "status": STRING},
                 outputs={"value": STRING},
                 parameters={"template": "{status}: {invoice} {{literal}}"}),
            output(),
        ]
        edges = [edge("request", "check"), edge("check", "amount"), edge("amount", "large", in_port="left"),
                 edge("limit", "large", in_port="right"), edge("large", "status", in_port="condition"),
                 edge("review", "status", in_port="if_true"), edge("auto", "status", in_port="if_false"),
                 edge("check", "note", in_port="invoice"), edge("status", "note", in_port="status"),
                 edge("note", "result")]
        saved = self.save(blueprint(*nodes, edges=edges, name="Invoice review"))
        for amount, status in ((1500, "Needs review"), (40.5, "Approved")):
            record = {"id": "INV-1", "amount": amount, "po": "PO-9"}
            result, _ = self.terminal(self.start(saved, {"inputs": {"request": record}}))
            self.assertEqual(result["status"], "passed", result.get("error"))
            self.assertEqual(result["results"]["result"],
                             f"{status}: {json.dumps(record, ensure_ascii=False)} {{literal}}")
        result, _ = self.terminal(self.start(saved, {"inputs": {"request": {"id": "INV-2", "amount": 3}}}))
        self.assertEqual((result["error"]["node_id"], result["error"]["code"], result["error"]["path"]),
                         ("check", "invalid_value", "inputs.value.po"))
        steps = {step["node_id"]: step["status"] for step in result["steps"]}
        self.assertEqual((steps["check"], steps["note"]), ("failed", "skipped"))

    def test_new_operations_reject_bad_configuration(self):
        number, boolean = {"type": "number"}, {"type": "boolean"}
        cases = [
            node("t", "transform", "template", inputs={"name": STRING}, outputs={"value": STRING},
                 parameters={"template": "Hello {name} from {city}"}),
            node("t", "transform", "template", inputs={"name": STRING}, outputs={"value": STRING},
                 parameters={"template": "   "}),
            node("t", "transform", "compare", inputs={"left": STRING, "right": STRING},
                 outputs={"value": boolean}, parameters={"operator": "greater"}),
            node("t", "transform", "compare", inputs={"left": number, "right": number},
                 outputs={"value": boolean}, parameters={"operator": "roughly"}),
            node("t", "transform", "compare", inputs={"left": STRING, "right": number},
                 outputs={"value": boolean}, parameters={"operator": "contains"}),
            node("t", "transform", "choose", inputs={"condition": boolean, "if_true": STRING, "if_false": number},
                 outputs={"value": STRING}),
            node("t", "transform", "validate", inputs={"value": OBJECT}, outputs={"value": OBJECT},
                 parameters={"schema": {"type": "object", "required": ["missing"]}}),
            node("t", "transform", "validate", inputs={"value": STRING}, outputs={"value": STRING},
                 parameters={"schema": {"type": "integer"}}),
        ]
        for case in cases:
            with self.subTest(operation=case["operation"], parameters=case.get("parameters")):
                validation = self.client.post("/blueprints/validate", json=blueprint(case)).json()
                self.assertFalse(validation["valid"])
                self.assertTrue(any(issue.get("node_id") == "t" and issue["code"] in
                                    ("invalid_parameters", "invalid_ports", "incompatible_ports")
                                    for issue in validation["issues"]), validation["issues"])

    def test_compare_uses_json_equality_and_containment(self):
        from backend.blueprints.runner import compare

        self.assertTrue(compare("equals", 1, 1.0))
        self.assertFalse(compare("equals", True, 1))
        self.assertTrue(compare("not-equals", False, 0))
        self.assertTrue(compare("equals", {"a": [1]}, {"a": [1]}))
        self.assertTrue(compare("contains", "order 245", "245"))
        self.assertTrue(compare("contains", ["a", "b"], "b"))
        self.assertTrue(compare("less-or-equal", 2, 2))

    def test_transform_failure_and_consumer_handoff_validation(self):
        parse = node("parse", "transform", "parse-json", inputs={"value": STRING}, outputs={"value": OBJECT})
        get = node("field", "transform", "field", inputs={"value": {"type": "object",
                   "properties": {"name": STRING}, "required": ["name"]}},
                   outputs={"value": STRING}, parameters={"field": "name"})
        saved = self.save(blueprint(source(), parse, get, output(), edges=[
            edge("request", "parse"), edge("parse", "field"), edge("field", "result")]))
        for raw, failure in [("{", ("parse", "operation_failed")), ("{}", ("field", "invalid_value"))]:
            result, _ = self.terminal(self.start(saved, {"inputs": {"request": raw}}))
            self.assertEqual((result["error"]["node_id"], result["error"]["code"]), failure)
        result, _ = self.terminal(self.start(saved, {"inputs": {"request": '{"name":"Alice"}'}}))
        self.assertEqual(result["results"], {"result": "Alice"})
        malformed = self.client.post(f"/blueprints/{saved['id']}/runs",
                                     content=b'{"inputs":{"request":"\\ud800"}}',
                                     headers={"Content-Type": "application/json"})
        self.assertEqual(malformed.status_code, 422)

    def test_local_rows_are_validated_and_frozen_before_queued_run(self):
        self.register_model()
        ingest = self.client.post("/datasets/ingest", json={"name": "Rows", "content": "One\n\nTwo"}).json()
        schema = {"type": "array", "items": {"type": "object", "properties": {"text": STRING},
                                            "required": ["text"]}}
        dataset = node("dataset", "dataset", "dataset-rows", outputs={"rows": schema},
                       binding={"source": "local", "dataset_id": ingest["id"]})
        count = node("count", "transform", "count", inputs={"value": {"type": "array"}},
                     outputs={"value": INTEGER})
        graph = blueprint(dataset, count, output({"type": "number"}),
                          edges=[edge("dataset", "count", "rows"), edge("count", "result")])
        bad = deepcopy(graph)
        bad["nodes"][0]["outputs"]["rows"]["items"]["properties"]["text"] = INTEGER
        self.assertEqual(self.client.post("/blueprints/validate", json=bad).json()["issues"][0]["path"],
                         "binding.rows[0].text")
        inline = deepcopy(graph)
        inline["nodes"][0]["binding"] = {"source": "inline", "rows": [{"text": "Inline"}]}
        result, _ = self.terminal(self.start(self.save(inline), {}))
        self.assertEqual(result["results"], {"result": 1})

        running = self.start(self.save(model_graph()), model_request(delay=5000))
        self.wait_for_node(running, "generate")
        self.assertEqual(self.client.post("/updates/quiesce").status_code, 409)
        saved = self.save(graph)
        path = self.start(saved, {"mode": "local"})
        queued = self.start(saved, {})
        self.assertEqual(self.client.post(f"{queued}/cancel").json()["status"], "cancelled")
        cancelled_queued, _ = self.terminal(queued)
        self.assertTrue(all(step["status"] == "skipped" for step in cancelled_queued["steps"]))
        self.assertEqual(self.client.delete(f"/datasets/{ingest['id']}").status_code, 204)
        self.assertEqual(self.client.post(f"{running}/cancel").json()["status"], "cancelled")
        cancelled, events = self.terminal(running)
        self.assertEqual([s["status"] for s in cancelled["steps"]], ["passed", "cancelled", "skipped"])
        self.assertEqual(events[-1][2]["status"], "cancelled")
        self.assertEqual(self.client.post(f"{running}/cancel").json()["status"], "cancelled")
        result, _ = self.terminal(path)
        self.assertEqual(result["mode"], "local")
        self.assertEqual(result["results"], {"result": 2})
        data = result["snapshot"]["bindings"]["datasets"][ingest["id"]]
        self.assertEqual(data["rows"], [{"text": "One"}, {"text": "Two"}])
        self.assertEqual(len(data["sha256"]), 64)

    def test_tool_fixture_receipt_approval_and_denial(self):
        approval = node("approval", "tool", "approval", inputs={"value": STRING}, outputs={"receipt": OBJECT})
        graph = blueprint(source(), approval, output(OBJECT),
                          edges=[edge("request", "approval"), edge("approval", "result", "receipt")])
        saved = self.save(graph)
        marker = self.home / "would-be-side-effect"
        for decision, expected in [("approved", "passed"), ("denied", "failed"), ("bad", "failed")]:
            path = self.start(saved, {"inputs": {"request": str(marker)}, "fixtures": {"approval": {
                "outputs": {"receipt": {"decision": decision, "target": str(marker)}}
            }}})
            result, _ = self.terminal(path)
            self.assertEqual(result["status"], expected)
            self.assertEqual(result["steps"][1]["outputs"]["receipt"]["decision"], decision)
        self.assertFalse(marker.exists())

    def test_saved_order_graph_runs_with_typed_fixtures(self):
        self.register_model()
        saved = self.save(order_blueprint(MODEL_ID))
        for sample in order_samples():
            result, _ = self.terminal(self.start(saved, sample["request"]))
            self.assertEqual(result["results"], sample["expected"])
        sample = deepcopy(order_samples()[0]["request"])
        sample["fixtures"]["select"]["outputs"]["index"] = 99
        failed, _ = self.terminal(self.start(saved, sample))
        self.assertEqual(failed["error"]["node_id"], "lookup")

    def test_local_run_reports_unsupported_checkpoint_before_admission(self):
        self.register_model()
        graph = model_graph()
        saved = self.save(graph)
        report = self.client.post("/blueprints/capabilities", json=graph)
        # The registry accepts our dummy checkpoint for fixtures. Real runs also
        # require the engine and a loadable local configuration.
        self.assertEqual(report.status_code, 422)
        self.assertIn(report.json()["detail"][0]["code"], ("unsupported_checkpoint", "runtime_unavailable"))
        rejected = self.client.post(f"/blueprints/{saved['id']}/runs",
                                    json={"mode": "local", "inputs": {"request": "hello"}})
        self.assertEqual(rejected.status_code, 422)
        self.assertEqual(self.client.get("/blueprints/runs").json(), [])

    def test_node_training_uses_runtime_schema_and_pins_checkpoint(self):
        weights = self.register_model()
        saved = self.save(order_blueprint(MODEL_ID))
        path = f"/blueprints/{saved['id']}/nodes/select/training"
        example = deepcopy(training_examples()["select"][0])
        bad = {"inputs": example["inputs"], "outputs": {"index": True}}

        class Tokenizer:
            eos_token_id = 2

            def encode(self, text, **kwargs):
                return [1] * len(text.split())

        checkpoint = SimpleNamespace(tokenizer=Tokenizer(), context=2048, model=ModelSnapshot(
            id=MODEL_ID, ref="local-test/checkpoint", path=str(weights), tokenizer_sha256="test-tokenizer"))
        job = TrainingJob(id="job", name="Test", status="queued", base_model=str(weights), dataset_id="unused")
        with patch("backend.blueprints.training.read_checkpoint", return_value=checkpoint), \
                patch.object(_get_manager(), "start", return_value=job) as start:
            invalid = self.client.post(path, json={"examples": [bad]})
            self.assertEqual(invalid.status_code, 422)
            self.assertEqual(invalid.json()["detail"][0]["path"], "examples[0].outputs.index")
            self.assertEqual(self.client.get("/datasets").json(), [])
            started = self.client.post(path, json={"examples": [example]})
            self.assertEqual(started.status_code, 202, started.text)
            cfg = start.call_args.args[0]
        self.assertEqual(cfg.base_model, str(weights))
        self.assertEqual(cfg.blueprint_node.model_id, MODEL_ID)
        self.assertEqual(cfg.blueprint_node.blueprint_id, saved["id"])
        self.assertEqual(cfg.blueprint_node.node_id, "select")
        record = _get_datasets().get(cfg.dataset_id)
        messages = json.loads(Path(record.path).read_text(encoding="utf-8"))["messages"]
        self.assertEqual(messages[-2]["content"], example["inputs"]["message"])
        self.assertEqual(messages[-1]["content"], str(example["outputs"]["index"]))
        from backend.blueprints.training import example_tokens

        ids, labels = example_tokens(Tokenizer(), messages)
        self.assertEqual(labels[:-2], [-100] * (len(ids) - 2))
        self.assertEqual(labels[-2:], [1, Tokenizer.eos_token_id])

    def test_startup_marks_unfinished_runs_and_repairs_step_states_once(self):
        from backend.routes.blueprints import blueprint_runner

        saved = self.save(simple_graph())
        store = blueprint_runner().store
        revision = store.revision(saved["id"])
        snapshot = RunSnapshot(blueprint=revision.blueprint, bindings=Bindings(),
                               request=RunRequest(inputs={"request": "input"}))
        records = []
        for status in ("running", "queued"):
            run = RunSummary(id=uuid4().hex, blueprint_id=revision.id, revision=revision.revision,
                             name=revision.blueprint.name, status=status, created_at=now())
            store.create_run(run, snapshot, [Step(node_id="request"), Step(node_id="result")])
            if status == "running":
                store.transition(run, [Step(node_id="request", status="running", started_at=now())])
            records.append(run)
        self.close_client()
        self.open_client()
        for run in records:
            path = f"/blueprints/runs/{run.id}"
            state, events = self.terminal(path)
            self.assertEqual(state["status"], "failed")
            self.assertEqual(state["error"]["code"], "interrupted")
            self.assertEqual(state["steps"][0]["status"], "failed" if run.status == "running" else "skipped")
            self.assertEqual(state["steps"][1]["status"], "skipped")
            self.assertEqual(sum(1 for _, kind, p in events if kind == "run" and p["status"] == "failed"), 1)
        self.close_client()
        self.open_client()
        self.assertFalse(blueprint_runner().has_active())

    def test_storage_refuses_unknown_database_version(self):
        database = str(self.home / "future.db")
        with connection(database) as conn:
            conn.execute("PRAGMA user_version = 99")
        with self.assertRaisesRegex(RuntimeError, "storage version: 99"):
            BlueprintStore(database)
        with connection(database) as conn:
            self.assertEqual(conn.execute("PRAGMA user_version").fetchone()[0], 99)


if __name__ == "__main__":
    unittest.main()
