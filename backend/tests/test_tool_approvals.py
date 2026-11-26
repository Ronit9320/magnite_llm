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
from backend._features import BLUEPRINTS_AND_TOOLS
from backend.server import app
from backend.tooling import pi_bridge
from backend.tooling.actions import Rejected, prepare
from backend.tooling.approvals import ApprovalConflict, Decision

TOKEN = "approval-test-token"
ROOT = "/blueprints/runs"


@unittest.skipUnless(BLUEPRINTS_AND_TOOLS, "Blueprints and Tool mode are switched off in this build")
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
    def fake_agent(value, engine, approve, stop, **_options):
        proposals = json.loads(value[-1]["content"])
        receipt = None
        for proposal in proposals:
            try:
                receipt = approve(proposal)
            except (ValueError, OSError):
                receipt = {"status": "rejected"}
        return receipt["status"]

    def stored(self, run_id):
        with connection(blueprint_runner().store.path) as conn:
            rows = [row[0] for row in conn.execute("SELECT audit FROM tool_approvals WHERE run_id = ?", (run_id,))]
            rows += [row[0] for row in conn.execute(
                "SELECT payload FROM events WHERE run_id = ? AND kind = 'approval'", (run_id,))]
        return json.dumps(rows)

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

    def test_command_output_reaches_the_run_but_not_the_audit(self):
        proposal = {"action": "command", "executable": str(Path(sys.executable).resolve()),
                    "arguments": ["-c", "print('output-marker'); raise SystemExit(3)"]}
        path = self.start("blueprints", proposal)
        self.assertEqual(self.decide(path, self.pending(path)).status_code, 200)
        run = self.terminal(path)
        self.assertEqual(run["status"], "failed")
        audit = self.client.get(path + "/approvals").json()[0]
        self.assertEqual((audit["status"], audit["outcome"]), ("failed", "nonzero_exit"))
        self.assertNotIn("output-marker", self.stored(audit["run_id"]))

        path = self.start("blueprints", {**proposal, "arguments": ["-c", "print('x' * 40000)"]})
        self.assertEqual(self.decide(path, self.pending(path)).status_code, 200)
        receipt = self.terminal(path)["results"]["result"]
        self.assertEqual((receipt["exit_code"], receipt["outcome"]), (0, "completed"))
        self.assertIn("bytes omitted", receipt["output"])
        self.assertLess(len(receipt["output"]), 17_000)

    def test_output_is_listed_for_the_user_but_never_stored(self):
        for script, printed, code in (("print('output-marker'); raise SystemExit(3)", "output-marker", 3),
                                      ("pass", "", 0)):
            with self.subTest(code=code):
                path = self.start("blueprints", {"action": "command", "executable": str(Path(sys.executable).resolve()),
                                                 "arguments": ["-c", script]})
                self.assertEqual(self.decide(path, self.pending(path)).status_code, 200)
                self.terminal(path)
                record = self.client.get(path + "/approvals").json()[0]
                self.assertEqual((record["output"].strip(), record["exit_code"]), (printed, code))
                self.assertNotIn("output-marker", self.stored(record["run_id"]))

    def test_only_recent_runs_keep_output_and_writes_keep_none(self):
        python = str(Path(sys.executable).resolve())
        scratch = tempfile.TemporaryDirectory()  # Writes inside MAGNITE_HOME are refused.
        self.addCleanup(scratch.cleanup)
        target = str(Path(scratch.name).resolve() / "kept-none.txt")
        paths = []
        with patch("backend.tooling.approvals.MAX_OUTPUT_RUNS", 1):
            for proposal in ({"action": "command", "executable": python, "arguments": ["-c", "print(1)"]},
                             {"action": "command", "executable": python, "arguments": ["-c", "print(2)"]},
                             {"action": "write", "target": target, "content": "x"}):
                paths.append(self.start("blueprints", proposal))
                self.assertEqual(self.decide(paths[-1], self.pending(paths[-1])).status_code, 200)
                self.terminal(paths[-1])
        first, second, written = (self.client.get(path + "/approvals").json()[0] for path in paths)
        self.assertNotIn("output", first)
        self.assertEqual(second["output"].strip(), "2")
        self.assertNotIn("output", written)

    def test_read_returns_file_text_and_folder_listing_after_approval(self):
        (self.home / "notes.md").write_text("todo: ship")
        (self.home / "sub").mkdir()
        for target, expected in (("notes.md", "todo: ship"), (".", "sub/")):
            path = self.start("blueprints", {"action": "read", "target": target})
            pending = self.pending(path)
            self.assertEqual(pending["kind"], "read")
            self.assertEqual(self.decide(path, pending).status_code, 200)
            receipt = self.terminal(path)["results"]["result"]
            self.assertIn(expected, receipt["output"])
            record = self.client.get(path + "/approvals").json()[0]
            self.assertIn(expected, record["output"])
            self.assertNotIn(expected, self.stored(record["run_id"]))

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
                        self.assertEqual([record["status"] for record in audit], ["rejected"])
                    else:
                        self.assertEqual(audit[0]["status"], expected)
                    self.assertFalse(self.marker.exists())
                    if surface == "blueprints":
                        self.assertEqual(terminal["status"], "cancelled" if expected == "cancelled" else "failed")

    def test_a_rejected_action_is_listed_but_never_persisted(self):
        path = self.start("chat", {"action": "read", "target": "missing.md"})
        self.terminal(path)
        record = self.client.get(path + "/approvals").json()[0]
        self.assertEqual((record["status"], record["kind"], record["outcome"]), ("rejected", "read", "not_found"))
        self.assertEqual(record["action"], {"action": "read", "target": "missing.md"})
        self.assertNotIn("missing.md", self.client.get(path + "/events").text)
        self.assertEqual(self.client.post(f"{path}/approvals/{record['id']}/decision",
                                          json={"decision": "approve_once", "fingerprint": "0" * 64}).status_code, 404)
        with connection(blueprint_runner().store.path) as conn:
            self.assertEqual(conn.execute("SELECT COUNT(*) FROM tool_approvals WHERE run_id = ?",
                                          (record["run_id"],)).fetchone()[0], 0)

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


class ReplyParsingTests(unittest.TestCase):
    def test_native_tags_fences_legacy_json_and_thinking(self):
        call = {"name": "run_command", "arguments": {"command": "ls -la"}}
        cases = {
            '<tool_call>\n{"name": "run_command", "arguments": {"executable": "ls", "arguments": ["-la"]}}\n</tool_call>':
                ("", [call]),
            '<think>\nI should list files.\n</think>\n\n<tool_call>{"name":"run_command","arguments":'
            '{"executable":"ls","arguments":["-la"]}}</tool_call>': ("", [call]),
            '```json\n{"name":"run_command","arguments":{"executable":"ls","arguments":["-la"]}}\n```': ("", [call]),
            '{"action":"command","executable":"ls","arguments":["-la"]}': ("", [call]),
            'Sure. {"name":"run_command","arguments":"{\\"executable\\":\\"ls\\",\\"arguments\\":[\\"-la\\"]}"}':
                ("", [call]),
            "[run_command(command='ls -la')]": ("", [call]),
            "read_file(path='notes.md')": ("", [{"name": "read_file", "arguments": {"path": "notes.md"}}]),
            "print(open('x').read())": ("print(open('x').read())", []),
            "None of the tools are needed for this. 17 times 23 is 391.": ("17 times 23 is 391.", []),
            '{"answer":"Paris"}': ("Paris", []),
            "<think>hmm</think>Paris is the capital.": ("Paris is the capital.", []),
            "<think>never finished": ("", []),
            '{"name":"delete_everything","arguments":{}}': ('{"name":"delete_everything","arguments":{}}', []),
        }
        for raw, expected in cases.items():
            with self.subTest(raw=raw):
                self.assertEqual(pi_bridge.parse_reply(raw, True), expected)

    def test_tools_off_keeps_calls_as_data(self):
        raw = '{"action":"open","target":"https://example.com"}'
        self.assertEqual(pi_bridge.parse_reply(raw, False), (raw, []))
        self.assertEqual(pi_bridge.parse_reply('Done.<tool_call>{"name":"open_target"}</tool_call>', False),
                         ("Done.", []))

    def test_tool_calls_become_exact_broker_actions(self):
        cases = [
            # Windows commands are split without POSIX quote rules (see pi_bridge.proposal).
            ({"name": "run_command", "arguments": {"command": "python3 -c 'print(1)'"}},
             {"action": "command", "executable": "python3",
              "arguments": ["-c", "print(1)" if os.name != "nt" else "'print(1)'"]}),
            ({"name": "read_file", "arguments": {"path": "notes.md"}}, {"action": "read", "target": "notes.md"}),
            ({"name": "list_folder", "arguments": {}}, {"action": "read", "target": "."}),
            ({"name": "open_target", "arguments": {"target": "https://example.com"}},
             {"action": "open", "target": "https://example.com"}),
        ]
        for call, expected in cases:
            with self.subTest(call=call):
                self.assertEqual(pi_bridge.proposal(call), expected)
        for call in ({"name": "run_command", "arguments": {"command": "  "}},
                     {"name": "run_command", "arguments": {"command": "echo 'open"}},
                     {"name": "delete", "arguments": {}}):
            with self.subTest(call=call), self.assertRaises(ValueError):
                pi_bridge.proposal(call)

    def test_a_tool_name_followed_by_its_arguments_is_a_call(self):
        # Seen from Qwen3 0.6B deep into a conversation; it was shown to the user as text.
        one = 'run_command\n{"command": "python3 --version"}'
        three = one + '\nrun_command\n{"command": "git --version"}\nrun_command\n{"command": "df -h"}'
        call = lambda command: {"name": "run_command", "arguments": {"command": command}}
        expected = {one: [call("python3 --version")],
                    three: [call("python3 --version"), call("git --version"), call("df -h")]}
        for raw, calls in expected.items():
            with self.subTest(raw=raw):
                self.assertEqual(pi_bridge.parse_reply(raw, True), ("", calls))
                self.assertEqual(pi_bridge.call_problem(raw, False), ("action_limit", "run_command"))
        for answer in ("To check, use:\n" + one, one + "\nThat is how the call looks.",
                       'delete_all\n{"path": "a"}'):
            with self.subTest(answer=answer):
                self.assertEqual(pi_bridge.parse_reply(answer, True), (answer, []))
                self.assertIsNone(pi_bridge.call_problem(answer, False))

    def test_every_call_in_a_reply_is_returned_once(self):
        tagged = lambda command: ('<tool_call>{"name":"run_command","arguments":{"command":"%s"}}</tool_call>'
                                  % command)
        text, calls = pi_bridge.parse_reply(tagged("a") + tagged("b") + tagged("a"), True)
        self.assertEqual((text, [call["arguments"]["command"] for call in calls]), ("", ["a", "b"]))
        self.assertEqual(pi_bridge.parse_reply("Use " + tagged("a"), False), ("Use", []))

    def test_a_list_of_python_style_calls_runs_each_and_never_crashes(self):
        call = lambda command: {"name": "run_command", "arguments": {"command": command}}
        raw = "[run_command(command='a'), run_command(command='b'), run_command(command='a')]"
        self.assertEqual(pi_bridge.parse_reply(raw, True), ("", [call("a"), call("b")]))
        for answer in ("run_command(command='a'), print('b')", "[run_command(command='a'), delete_all()]",
                       "run_command(command='a') then run_command(command='b')", "read_file(path={[]})"):
            with self.subTest(answer=answer):
                self.assertEqual(pi_bridge.parse_reply(answer, True), (answer, []))

    def test_several_json_calls_on_their_own_lines_are_each_a_call(self):
        # The form a model without a tool format sees for its own earlier calls.
        line = lambda command: json.dumps({"name": "run_command", "arguments": {"command": command}})
        call = lambda command: {"name": "run_command", "arguments": {"command": command}}
        two = line("a") + "\n" + line("b")
        for raw in (two, f"```json\n{two}\n```", two + "\n" + line("a")):
            with self.subTest(raw=raw):
                self.assertEqual(pi_bridge.parse_reply(raw, True), ("", [call("a"), call("b")]))
        for answer in ("Use these:\n" + two, two + "\nThat is how they look.", '{"name":"Alice"}\n{"name":"Bob"}'):
            with self.subTest(answer=answer):
                self.assertEqual(pi_bridge.parse_reply(answer, True), (answer, []))
                self.assertIsNone(pi_bridge.call_problem(answer, True))
        mixed = line("a") + '\n{"name":"delete_all","arguments":{}}'
        self.assertEqual(pi_bridge.parse_reply(mixed, True), (mixed, []))
        self.assertEqual(pi_bridge.call_problem(mixed, True), ("unknown_tool", "delete_all"))

    def test_the_examples_name_each_platform_s_own_commands(self):
        example = lambda ask, command: ('%s -> {"name": "run_command", "arguments": {"command": "%s"}}'
                                        % (ask, command))
        for platform, specs, memory in (("darwin", "system_profiler SPHardwareDataType", "top -l 1 -n 0"),
                                        ("linux", "lscpu", "free -h"), ("win32", "systeminfo", "systeminfo")):
            with self.subTest(platform=platform), patch("backend.tooling.pi_bridge.sys.platform", platform):
                self.assertEqual(pi_bridge.system_prompt("/work").splitlines()[-2:],
                                 [example("what are this computer's specs", specs),
                                  example("how much memory is in use", memory)])

    def test_write_file_becomes_a_write_action(self):
        self.assertEqual(pi_bridge.proposal({"name": "write_file", "arguments": {"path": "a.md", "content": "hi"}}),
                         {"action": "write", "target": "a.md", "content": "hi"})

    def test_unquoted_shell_syntax_is_refused_and_quoted_text_is_not(self):
        call = lambda command: {"name": "run_command", "arguments": {"command": command}}
        for command in ("ls | wc", "echo hi > ~/test.md", "a && b", "a;b", "sleep 1 &", "cat < x"):
            with self.subTest(command=command), self.assertRaises(Rejected) as caught:
                pi_bridge.proposal(call(command))
            self.assertEqual(caught.exception.reason, "shell_syntax")
        allowed = ["grep 'a|b' notes.txt", 'python3 -c "print(1>0)"']
        if os.name != "nt":
            allowed.append(r"find . -exec cat {} \;")
        for command in allowed:
            with self.subTest(command=command):
                self.assertEqual(pi_bridge.proposal(call(command))["action"], "command")

    def test_a_command_given_as_a_list_cannot_slip_shell_syntax_past_the_check(self):
        split = lambda *parts: ('<tool_call>{"name":"run_command","arguments":{"executable":"%s","arguments":%s}}'
                                "</tool_call>" % (parts[0], json.dumps(list(parts[1:]))))
        for parts in (("echo", "hi", ">", "f.txt"), ("ls", "|", "wc"), ("a", "&&", "b"), ("x", "2>&1"),
                      ("echo", "a", ";", "b")):
            with self.subTest(parts=parts), self.assertRaises(Rejected):
                pi_bridge.proposal(pi_bridge.parse_reply(split(*parts), True)[1][0])
        for parts in (("grep", "a|b", "notes.txt"), ("find", ".", "-exec", "cat", "{}", ";")):
            with self.subTest(parts=parts):
                action = pi_bridge.proposal(pi_bridge.parse_reply(split(*parts), True)[1][0])
                self.assertEqual(action["arguments"], list(parts[1:]))

    def test_a_leading_tilde_is_expanded_and_shown(self):
        action = pi_bridge.proposal({"name": "run_command", "arguments": {"command": "touch ~/x.txt a~b"}})
        self.assertEqual(action["arguments"], [os.path.expanduser("~/x.txt"), "a~b"])

    def test_a_missing_file_receipt_names_only_the_reason(self):
        # A hint meant for the model was repeated to the user.
        text = pi_bridge.receipt_text({"status": "rejected", "outcome": "not_found"})
        self.assertIn("not run", text)
        self.assertIn("does not exist", text)
        self.assertNotIn("write_file", text)

    def test_unknown_and_incomplete_calls_are_named(self):
        cases = [
            ('<tool_call>{"name":"delete_all","arguments":{}}</tool_call>', True, ("unknown_tool", "delete_all")),
            ('{"name":"delete_all","arguments":{}}', True, ("unknown_tool", "delete_all")),
            ('{"name":"list_folder","arguments":{}', True, ("incomplete_call", "")),
            ('<tool_call>\n{"name": "read_', True, ("incomplete_call", "")),
            ('{"name":"read_file","arguments":{"path":"a"}}', False, ("action_limit", "read_file")),
            ('{"name":"Alice","age":3}', True, None),
            ('{"name": "Alice", "age": 30}\n\nThat is the JSON object you asked for.', True, None),
            ('{"name":"Alice"}\n{"name":"Bob"}', True, None),
            ('{"name":"Alice","age":30,}', True, None),
            ('{"name": "magnite", "version": "1.0.0"}\nSave this as package.json.', True, None),
            ('{"answer":"hi"}', True, None),
            ('A call looks like {"name":"x","arguments":{}} in this format.', True, None),
            ("Paris is the capital.", True, None),
        ]
        for raw, offered, expected in cases:
            with self.subTest(raw=raw):
                self.assertEqual(pi_bridge.call_problem(raw, offered), expected)

    def test_receipts_describe_output_and_refusals(self):
        text = pi_bridge.receipt_text({"status": "completed", "outcome": "completed", "exit_code": 0,
                                       "output": "Python 3.13.1\n"})
        self.assertIn("Exit code: 0", text)
        self.assertIn("Python 3.13.1", text)
        self.assertIn("denied", pi_bridge.receipt_text({"status": "denied"}))
        self.assertIn("not run", pi_bridge.receipt_text({"status": "rejected", "outcome": "invalid_action"}))


class PiBridgeTests(unittest.TestCase):
    def setUp(self):
        try:
            pi_bridge.node_path()
        except (ValueError, OSError) as exc:
            # CI sets MAGNITE_REQUIRE_PI so a missing runtime is a failure, not a silent skip.
            if os.environ.get("MAGNITE_REQUIRE_PI"):
                self.fail(f"MAGNITE_REQUIRE_PI is set but the Pi runtime is unavailable: {exc}")
            self.skipTest("Install the optional pinned Pi runtime to exercise its subprocess")

    def test_native_tool_calls_round_trip_with_output_and_four_proposal_limit(self):
        seen = []

        def generate(messages, tools, engine, stop, native):
            seen.append((messages, tools))
            if len(seen) == 1:
                self.assertEqual(messages[0]["role"], "system")
                self.assertIn("/work", messages[0]["content"])
                return ('<tool_call>{"name":"run_command","arguments":{"executable":"python3",'
                        '"arguments":["--version"]}}</tool_call>')
            if len(seen) == 2:
                self.assertEqual(messages[-2]["tool_calls"][0]["function"],
                                 {"name": "run_command", "arguments": {"command": "python3 --version"}})
                self.assertEqual(messages[-1]["role"], "tool")
                self.assertIn("Python 3.13.1", messages[-1]["content"])
                return "You have Python 3.13.1."
            raise AssertionError("unexpected turn")

        calls = []

        def approve(proposal):
            calls.append(proposal)
            return {"status": "completed", "outcome": "completed", "exit_code": 0, "output": "Python 3.13.1"}

        with patch("backend.tooling.pi_bridge._generate", side_effect=generate):
            result = pi_bridge.run("Which Python do I have?", object(), approve, Event(), working_directory="/work")
        self.assertEqual(result, "You have Python 3.13.1.")
        self.assertEqual(calls, [{"action": "command", "executable": "python3", "arguments": ["--version"]}])

    def test_every_tool_turn_has_exactly_one_leading_system_message(self):
        # Pi adds its own empty system message unless it is given the prompt; templates
        # such as Mistral and Gemma reject a second system message.
        turns = []

        def generate(messages, tools, engine, stop, native):
            turns.append(messages)
            if len(turns) == 1:
                return '{"name":"read_file","arguments":{"path":"notes.md"}}'
            return "Done."

        approve = lambda proposal: {"status": "completed", "outcome": "completed", "output": "text"}
        with patch("backend.tooling.pi_bridge._generate", side_effect=generate):
            pi_bridge.run("Read my notes", object(), approve, Event(), working_directory="/work")
        self.assertEqual(len(turns), 2)
        for messages in turns:
            self.assertEqual(sum(m["role"] == "system" for m in messages), 1)
            self.assertEqual(messages[0]["role"], "system")
            self.assertIn("/work", messages[0]["content"])

    def test_a_tool_call_cut_off_at_the_token_cap_is_reported_not_answered(self):
        engine = SimpleNamespace(count_tokens=lambda text: pi_bridge.TURN_TOKENS)
        cut = '<tool_call>\n{"name": "run_command", "arguments": {"command": "python3 -'
        approve = lambda proposal: self.fail("a cut-off call must never be proposed")
        with patch("backend.tooling.pi_bridge._generate", return_value=cut):
            with self.assertRaisesRegex(RuntimeError, "cut off"):
                pi_bridge.run("Check Python", engine, approve, Event())

    def test_a_json_protocol_call_cut_off_at_the_token_cap_is_reported(self):
        engine = SimpleNamespace(count_tokens=lambda text: pi_bridge.TURN_TOKENS)
        with patch("backend.tooling.pi_bridge._generate", return_value='{"name":"read_file","arguments":{"pa'):
            with self.assertRaisesRegex(RuntimeError, "cut off"):
                pi_bridge.run("Read it", engine, lambda proposal: self.fail("no proposal"), Event())

    def test_a_long_answer_cut_off_at_the_token_cap_says_so(self):
        engine = SimpleNamespace(count_tokens=lambda text: pi_bridge.TURN_TOKENS)
        with patch("backend.tooling.pi_bridge._generate", return_value="The first half of a long answer"):
            result = pi_bridge.run("Explain", engine, lambda proposal: {}, Event())
        self.assertTrue(result.startswith("The first half of a long answer"))
        self.assertIn("cut off", result)

    def test_an_answer_under_the_cap_is_returned_unchanged(self):
        engine = SimpleNamespace(count_tokens=lambda text: 12)
        with patch("backend.tooling.pi_bridge._generate", return_value="Short answer."):
            self.assertEqual(pi_bridge.run("Explain", engine, lambda proposal: {}, Event()), "Short answer.")

    def test_denials_continue_until_the_action_budget_and_then_answer(self):
        turns = []

        def generate(messages, tools, engine, stop, native):
            turns.append(tools)
            if len(turns) <= 5:
                return '{"name":"open_target","arguments":{"target":"https://example.com"}}'
            self.assertIn("action limit", messages[-1]["content"])
            self.assertEqual(sum("denied" in m["content"] for m in messages if m["role"] == "tool"), 4)
            return "The proposed actions were denied."

        calls = []

        def deny(proposal):
            calls.append(proposal)
            return {"status": "denied"}

        refused = []
        with patch("backend.tooling.pi_bridge._generate", side_effect=generate):
            result = pi_bridge.run("Ask to open a website", object(), deny, Event(),
                                   reject=lambda *args: refused.append(args))
        self.assertEqual([(kind, reason) for kind, _, reason in refused], [("open", "action_limit")])
        self.assertIn("denied", result)
        self.assertEqual(calls, [{"action": "open", "target": "https://example.com"}] * 4)
        self.assertEqual(turns[-1], [])

    def test_several_calls_in_one_reply_are_each_proposed_up_to_the_limit(self):
        tagged = lambda n: '<tool_call>{"name":"run_command","arguments":{"command":"echo %d"}}</tool_call>' % n
        replies = iter(["".join(map(tagged, range(5))), "Done."])
        asked, refused = [], []

        def approve(proposal):
            asked.append(proposal["arguments"][0])
            return {"status": "denied" if len(asked) == 1 else "completed", "outcome": "completed"}

        with patch("backend.tooling.pi_bridge._generate", side_effect=lambda *args: next(replies)):
            result = pi_bridge.run("Echo", object(), approve, Event(),
                                   reject=lambda *args: refused.append(args[2]))
        self.assertEqual((asked, refused, result), (["0", "1", "2", "3"], ["action_limit"], "Done."))

    def test_a_call_that_cannot_run_beside_good_ones_is_still_shown(self):
        raw = ('<tool_call>{"name":"run_command","arguments":{"command":"echo 1"}}</tool_call>'
               '<tool_call>{"name":"delete_all","arguments":{}}</tool_call>'
               '<tool_call>{"name":"run_command","arguments":{"comm')
        replies = iter([raw, "Done."])
        asked, refused = [], []

        def approve(proposal):
            asked.append(proposal["arguments"][0])
            return {"status": "completed", "outcome": "completed"}

        with patch("backend.tooling.pi_bridge._generate", side_effect=lambda *args: next(replies)):
            pi_bridge.run("Echo", object(), approve, Event(),
                          reject=lambda kind, action, reason: refused.append((action["target"], reason)))
        self.assertEqual((asked, refused), (["1"], [("delete_all", "unknown_tool"), ("", "incomplete_call")]))

    def test_several_results_reach_a_model_without_a_tool_format_as_one_turn(self):
        # Templates such as Gemma and Mistral reject two user turns in a row.
        result = lambda text: {"role": "tool", "name": "run_command", "content": text}
        flat = pi_bridge._plain_messages(
            [{"role": "user", "content": "Check"}, {"role": "assistant", "content": "", "tool_calls": [
                {"function": {"name": "run_command", "arguments": {"command": "a"}}},
                {"function": {"name": "run_command", "arguments": {"command": "b"}}}]},
             result("first-output"), result("second-output")], [])
        self.assertEqual([message.role.value for message in flat], ["user", "assistant", "user"])
        self.assertEqual(flat[-1].content, "Tool result for run_command:\nfirst-output\n\n"
                                           "Tool result for run_command:\nsecond-output")

    def test_a_refusal_before_approval_is_reported_to_the_caller(self):
        replies = iter(['{"name":"run_command","arguments":{"command":"ls | wc"}}', "Done."])
        refused = []
        with patch("backend.tooling.pi_bridge._generate", side_effect=lambda *args: next(replies)):
            pi_bridge.run("List", object(), lambda proposal: self.fail("never proposed"), Event(),
                          reject=lambda *args: refused.append(args))
        self.assertEqual([(kind, reason) for kind, _, reason in refused], [("command", "shell_syntax")])

    def test_an_unknown_tool_gets_a_plain_message_and_a_record(self):
        refused = []
        with patch("backend.tooling.pi_bridge._generate",
                   return_value='<tool_call>{"name":"delete_all","arguments":{}}</tool_call>'):
            result = pi_bridge.run("Clean up", object(), lambda p: self.fail("never"), Event(),
                                   reject=lambda *args: refused.append(args))
        self.assertEqual(result, "Nothing ran: the model asked for a tool Magnite does not have (delete_all).")
        self.assertEqual([(kind, reason) for kind, _, reason in refused], [("unknown", "unknown_tool")])

    def test_reminders_reach_the_model_only(self):
        for status, reminded in (("completed", True), ("denied", False), ("failed", False)):
            turns = []
            replies = iter(['{"name":"read_file","arguments":{"path":"a"}}',
                            "Done. " + pi_bridge.STEP_REMINDER])

            def generate(messages, tools, engine, stop, native):
                turns.append([dict(message) for message in messages])
                return next(replies)

            with self.subTest(status=status), patch("backend.tooling.pi_bridge._generate", side_effect=generate):
                result = pi_bridge.run("Read my notes", object(),
                                       lambda p: {"status": status, "outcome": status}, Event())
                self.assertEqual(turns[0][-1]["content"], "Read my notes\n\n" + pi_bridge.REQUEST_REMINDER)
                self.assertEqual(turns[1][-1]["content"].endswith(pi_bridge.STEP_REMINDER), reminded)
                self.assertEqual(result, "Done.")

    def test_text_agent_passes_instructions_and_keeps_action_shaped_text_as_data(self):
        proposed = '{"action":"open","target":"https://example.com"}'

        def generate(messages, tools, engine, stop, native):
            self.assertEqual(messages, [{"role": "system", "content": "Summarize the request."},
                                        {"role": "user", "content": "A request"}])
            self.assertEqual((tools, native), ([], False))
            return proposed

        with patch("backend.tooling.pi_bridge._generate", side_effect=generate):
            self.assertEqual(pi_bridge.run("A request", object(), None, Event(),
                                           instructions="Summarize the request."), proposed)

    def test_cancelling_a_pi_proposal_stops_the_bridge(self):
        stop = Event()

        def generate(*args):
            return '{"name":"open_target","arguments":{"target":"https://example.com"}}'

        def cancel(_proposal):
            stop.set()
            return {"status": "cancelled"}

        with patch("backend.tooling.pi_bridge._generate", side_effect=generate):
            with self.assertRaisesRegex(RuntimeError, "cancelled"):
                pi_bridge.run("Ask to open a website", object(), cancel, stop)
