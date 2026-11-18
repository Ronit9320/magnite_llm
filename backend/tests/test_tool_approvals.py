"""Exercise the shared authenticated broker and its real command dispatch."""

from contextlib import ExitStack
import json
import os
from pathlib import Path
import sys
import tempfile
from threading import Event
from time import monotonic, sleep
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from fastapi.testclient import TestClient

from backend.blueprints.runner import BlueprintRunner
from backend.blueprints.store import BlueprintStore
from backend.database import connection
from backend.local_access import LocalAccessMiddleware
from backend.routes.blueprints import blueprint_runner
from backend.server import app
from backend.tooling import pi_bridge
from backend.tooling.actions import prepare
from backend.tooling.approvals import ApprovalConflict, Decision

TOKEN = "approval-test-token"
ROOT = "/blueprints/runs"


class ToolApprovalTests(unittest.TestCase):
    def setUp(self):
        resources = ExitStack()
        self.addCleanup(resources.close)
        self.home = Path(resources.enter_context(tempfile.TemporaryDirectory())).resolve()
        self.marker = self.home / "literal; marker.txt"
        resources.enter_context(patch.dict(os.environ, {"MAGNITE_HOME": str(self.home)}))
        resources.enter_context(patch("backend.runtime_packages.activate"))
        resources.enter_context(patch("backend.tooling.pi_bridge.loaded_engine",
                                      return_value=SimpleNamespace(name="torch", model_ref="test")))
        resources.enter_context(patch("backend.tooling.pi_bridge.run", side_effect=self.fake_agent))
        self.client = resources.enter_context(TestClient(
            LocalAccessMiddleware(app, TOKEN), base_url="http://127.0.0.1",
            headers={"Authorization": f"Bearer {TOKEN}"},
        ))
        object_schema = {"type": "object"}
        blueprint = {
            "version": 1, "name": "Tool test", "nodes": [
                {"id": "action", "kind": "input", "operation": "input", "outputs": {"value": object_schema}},
                {"id": "review", "kind": "tool", "operation": "approval", "inputs": {"value": object_schema},
                 "outputs": {"receipt": object_schema}},
                {"id": "result", "kind": "output", "operation": "output", "inputs": {"value": object_schema},
                 "outputs": {"value": object_schema}},
            ],
            "edges": [
                {"source": {"node": "action", "port": "value"}, "target": {"node": "review", "port": "value"}},
                {"source": {"node": "review", "port": "receipt"}, "target": {"node": "result", "port": "value"}},
            ],
        }
        response = self.client.post("/blueprints", json=blueprint)
        self.assertEqual(response.status_code, 201, response.text)
        self.blueprint_id = response.json()["id"]

    @staticmethod
    def fake_agent(value, engine, approve, stop):
        proposals = json.loads(value[-1]["content"])
        receipt = None
        for proposal in proposals:
            try:
                receipt = approve(proposal)
            except (ValueError, OSError):
                receipt = {"status": "rejected"}
        return receipt["status"]

    def command(self):
        script = "from pathlib import Path; p=Path(__import__('sys').argv[1]); " \
                 "p.write_text((p.read_text() if p.exists() else '') + 'ran')"
        return {"action": "command", "executable": str(Path(sys.executable).resolve()),
                "arguments": ["-c", script, self.marker.name]}

    def start(self, surface, proposal, *, repeated=False):
        cwd = str(self.home)
        if surface == "chat":
            proposals = [proposal, proposal] if repeated else [proposal]
            response = self.client.post("/inference/tool-runs", json={
                "working_directory": cwd,
                "messages": [{"role": "user", "content": json.dumps(proposals)}],
            })
        else:
            response = self.client.post(f"/blueprints/{self.blueprint_id}/runs", json={
                "mode": "local", "working_directory": cwd, "inputs": {"action": proposal},
            })
        self.assertEqual(response.status_code, 202, response.text)
        self.assertEqual(response.json()["surface"], surface)
        self.assertEqual(response.json()["tool_chat"], surface == "chat")
        return f"{ROOT}/{response.json()['id']}"

    def wait_for(self, check):
        deadline = monotonic() + 8
        while monotonic() < deadline:
            result = check()
            if result is not None:
                return result
            sleep(0.02)
        self.fail("Timed out waiting for the tool run")

    def pending(self, path):
        def find_pending():
            records = self.client.get(path + "/approvals").json()
            return next((record for record in records if record["status"] == "pending"), None)
        return self.wait_for(find_pending)

    def terminal(self, path):
        def find_terminal():
            run = self.client.get(path).json()
            return run if run["status"] not in ("queued", "running") else None
        return self.wait_for(find_terminal)

    def decide(self, path, item, decision="approve_once", **fields):
        return self.client.post(f"{path}/approvals/{item['id']}/decision",
                                json={"decision": decision, "fingerprint": item["fingerprint"], **fields})

    def test_approve_once_is_exact_and_audit_is_redacted_on_both_surfaces(self):
        old_path, old_item = None, None
        for surface in ("blueprints", "chat"):
            with self.subTest(surface=surface):
                path = self.start(surface, self.command())
                pending = self.pending(path)
                self.assertEqual(pending["action"]["arguments"], self.command()["arguments"])
                self.assertEqual(pending["action"]["working_directory"], str(self.home))
                self.assertFalse(self.marker.exists())
                self.assertEqual(self.decide(path, pending, fingerprint="0" * 64).status_code, 409)
                self.assertEqual(self.decide(path, pending, arguments=["changed"]).status_code, 422)
                if old_item:
                    self.assertNotEqual(pending["fingerprint"], old_item["fingerprint"])
                    self.assertEqual(self.decide(path, old_item).status_code, 404)
                    self.assertEqual(self.decide(old_path, old_item).status_code, 409)
                self.assertEqual(self.decide(path, pending).status_code, 200)
                self.assertEqual(self.terminal(path)["status"], "passed")
                self.assertEqual(self.decide(path, pending).status_code, 409)
                self.assertEqual(self.marker.read_text(), "ran")
                self.marker.unlink()
                audit = self.client.get(path + "/approvals").json()[0]
                self.assertEqual((audit["status"], audit["decision"]), ("completed", "approve_once"))
                self.assertNotIn("action", audit)
                events = self.client.get(path + "/events").text
                self.assertIn("event: approval", events)
                self.assertNotIn(self.marker.name, events)
                old_path, old_item = path, pending

    def test_denial_expiry_cancellation_and_malformed_actions_never_dispatch(self):
        for surface in ("blueprints", "chat"):
            for expected in ("denied", "expired", "cancelled", "malformed"):
                with self.subTest(surface=surface, expected=expected):
                    runner = blueprint_runner()
                    ttl = 0.1 if expected == "expired" else runner.broker.ttl
                    proposal = self.command()
                    if expected == "malformed":
                        proposal["shell"] = True
                    with patch.object(runner.broker, "ttl", ttl):
                        path = self.start(surface, proposal)
                        if expected in ("denied", "cancelled"):
                            pending = self.pending(path)
                            if expected == "denied":
                                self.assertEqual(self.decide(path, pending, "deny").status_code, 200)
                            else:
                                self.assertEqual(self.client.post(path + "/cancel").json()["status"], "cancelled")
                            self.assertEqual(self.decide(path, pending).status_code, 409)
                        terminal = self.terminal(path)
                    audit = self.client.get(path + "/approvals").json()
                    if expected == "malformed":
                        self.assertEqual(audit, [])
                    else:
                        self.assertEqual(audit[0]["status"], expected)
                    self.assertFalse(self.marker.exists())
                    if surface == "blueprints":
                        self.assertEqual(terminal["status"], "cancelled" if expected == "cancelled" else "failed")

    def test_repeated_chat_proposal_needs_a_new_approval(self):
        path = self.start("chat", self.command(), repeated=True)
        first = self.pending(path)
        self.assertEqual(self.decide(path, first, "deny").status_code, 200)
        second = self.pending(path)
        self.assertNotEqual(first["id"], second["id"])
        self.assertEqual(self.decide(path, first).status_code, 409)
        self.assertEqual(self.decide(path, second, "deny").status_code, 200)
        self.assertEqual(self.terminal(path)["results"], {"reply": {"text": "denied"}})
        self.assertFalse(self.marker.exists())

    def test_restart_closes_pending_approval_without_replaying_it(self):
        path = self.start("blueprints", self.command())
        item = self.pending(path)
        original = blueprint_runner()
        copied = str(self.home / "crash-snapshot.db")
        with connection(original.store.path) as src, connection(copied) as dst:
            src.backup(dst)
        restarted = BlueprintRunner(BlueprintStore(copied), original.resolver)
        try:
            run = restarted.store.run(item["run_id"])
            self.assertEqual(run.status, "failed")
            records = restarted.broker.list(run.id)
            self.assertEqual(records[0].status, "interrupted")
            self.assertIsNone(records[0].action)
            with self.assertRaises(ApprovalConflict):
                restarted.broker.decide(run.id, item["id"],
                                        Decision(decision="approve_once", fingerprint=item["fingerprint"]))
            events, terminal = restarted.store.events(run.id, 0)
            self.assertTrue(terminal)
            self.assertEqual([event["kind"] for event in events][-2:], ["approval", "run"])
            self.assertFalse(self.marker.exists())
        finally:
            restarted.shutdown()
            self.client.post(path + "/cancel")

    def test_external_actions_require_an_authenticated_launch(self):
        client = TestClient(LocalAccessMiddleware(app, None), base_url="http://127.0.0.1")
        self.addCleanup(client.close)
        self.assertEqual(client.get("/blueprints").status_code, 200)
        self.assertEqual(client.post("/inference/tool-runs", json={
            "messages": [{"role": "user", "content": "Open a folder"}],
        }).status_code, 403)
        self.assertEqual(client.post(f"{ROOT}/run/approvals/id/decision",
                                     json={"decision": "approve_once", "fingerprint": "0" * 64}).status_code, 403)
        self.assertEqual(client.post(f"/blueprints/{self.blueprint_id}/runs", json={
            "mode": "local", "inputs": {"action": self.command()},
        }).status_code, 403)

    def test_open_resolves_a_local_target_and_detects_file_replacement(self):
        self.marker.write_text("original")
        prepared = prepare({"action": "open", "target": self.marker.name}, self.home)
        self.assertEqual(prepared.display(), {"action": "open", "target": str(self.marker)})
        self.marker.write_text("different length after approval")
        with self.assertRaisesRegex(ValueError, "changed"):
            prepared.paths[0].verify()
        for target in ("file:///tmp/report", "https://user:secret@example.com", "http://invalid host"):
            with self.subTest(target=target), self.assertRaises(ValueError):
                prepare({"action": "open", "target": target}, self.home)


class PiBridgeTests(unittest.TestCase):
    def setUp(self):
        try:
            pi_bridge.node_path()
        except (ValueError, OSError):
            self.skipTest("Install the optional pinned Pi runtime to exercise its subprocess")

    def test_pinned_pi_issues_four_proposals_and_continues_with_receipts(self):
        calls = []

        async def reply(messages, engine, stop):
            if len(calls) == 4:
                self.assertIn("Magnite tool receipt", messages[-1].content)
                return '{"answer":"The proposed actions were denied."}'
            return '{"action":"open","target":"https://example.com"}'

        def deny(proposal):
            calls.append(proposal)
            return {"status": "denied"}

        with patch("backend.tooling.pi_bridge._local_reply", side_effect=reply):
            result = pi_bridge.run("Ask to open a website", object(), deny, Event())
        self.assertEqual(result, "The proposed actions were denied.")
        self.assertEqual(calls, [{"action": "open", "target": "https://example.com"}] * 4)

    def test_cancelling_a_pi_proposal_stops_the_bridge(self):
        stop = Event()

        async def reply(*args):
            return '{"action":"open","target":"https://example.com"}'

        def cancel(_proposal):
            stop.set()
            return {"status": "cancelled"}

        with patch("backend.tooling.pi_bridge._local_reply", side_effect=reply):
            with self.assertRaisesRegex(RuntimeError, "cancelled"):
                pi_bridge.run("Ask to open a website", object(), cancel, stop)
