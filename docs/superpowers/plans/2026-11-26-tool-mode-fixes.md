# Tool Mode Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Tool mode can create or replace a text file after approval, shows every attempted action, and gets models to use tools more reliably.

**Architecture:** The Pi worker (`worker.mjs`) does not change. The broker (`tooling/actions.py`, `tooling/approvals.py`) gains a `write` action and an in-memory list of rejected attempts. The bridge (`tooling/pi_bridge.py`) gains the tool, rejection reporting, reminders and output trimming. The Qt client renders the new cards.

**Tech Stack:** Python 3 (unittest, pydantic), Node 22.19+ with the pinned Pi packages, C++20 / Qt 6 Widgets.

**Spec:** `docs/superpowers/specs/2026-11-26-tool-mode-fixes-design.md`

## Global Constraints

- Branch `pi-harness-fixes`. **Do not commit or push unless the user asks.** If asked: one-line subject, at most two body lines, no attribution trailer. Leave `app/src/widgets/focusring.h`, `brag-output-*` and other untracked files alone.
- Nothing writes, runs or opens without an explicit approval.
- `MAX_TURNS = 6`, `MAX_PROPOSALS = 4`, `TURN_TOKENS = 768` stay as they are.
- File content: UTF-8, at most 16,000 bytes, written exactly; newline, carriage return and tab are the only control characters allowed; Unicode direction overrides (U+202A–U+202E, U+2066–U+2069) are rejected.
- Rejected attempts live in backend memory only. No `DATA_SCHEMA` bump.
- llama.cpp context: 8192, or the model's own limit if smaller.
- Never hardcode hex colours; use theme tokens and existing object names.
- Build in two limited steps: `cmake --build build --target magnite-app_autogen -j 4` then `cmake --build build --target magnite-app -j 4`.
- Model runs on this 8 GB Mac go through `backend/rag/memguard.py`. Running the app's torch selftest, or turning a guard off, needs the user's OK each time.

**Reason codes** (exact strings, shared by backend and client):

| Code | Card text |
|---|---|
| `not_found` | The file does not exist. |
| `folder_not_found` | The folder does not exist. |
| `not_a_file` | That is not a regular file. |
| `shell_syntax` | Shell syntax such as \| and > is not supported. |
| `protected_location` | Magnite's own data folder cannot be written to. |
| `invalid_request` | The request was not valid. |
| `action_limit` | The action limit for this task was reached. |
| `unknown_tool` | Magnite does not have that tool. |
| `incomplete_call` | The model's tool call was incomplete. |

## Review Focus

1. Empty content, CRLF, or no trailing newline: the file holds exactly those bytes (Task 1).
2. A symlink or a differently cased path leading into the app data folder: still refused (Task 1).
3. A target created or edited between approval and write: the action fails and nothing is overwritten (Task 1).
4. Quoted or escaped shell characters (`grep 'a|b'`, `find . -exec cat {} \;`): not rejected (Task 3).
5. A real answer that happens to be JSON with a `name` field: shown, not replaced by "Nothing ran" (Task 4).

---

### Task 1: The `write` broker action

**Files:**
- Modify: `backend/src/backend/tooling/actions.py`
- Modify: `backend/src/backend/tooling/approvals.py` (the `Approval.kind` literal)
- Create: `backend/tests/test_tool_write.py`

**Interfaces:**
- Produces: `class Rejected(ValueError)` with `reason: str`, constructed as `Rejected(reason)`; `CONTENT_LIMIT = 16_000`; action dict `{"action": "write", "target": str, "content": str}`; `PreparedAction` gains `content: str = ""` and `existing_bytes: int | None = None`; `display()` for a write returns `action, target, content, replaces` plus `existing_bytes` when replacing. `prepare()` raises only `Rejected` for bad requests.

- [ ] **Step 1: Write the failing tests**

```python
from contextlib import ExitStack
import os
from pathlib import Path
import tempfile
from threading import Event
import unittest
from unittest.mock import patch

from backend.tooling.actions import Rejected, execute, prepare


class WriteActionTests(unittest.TestCase):
    def setUp(self):
        resources = ExitStack()
        self.addCleanup(resources.close)
        root = Path(resources.enter_context(tempfile.TemporaryDirectory())).resolve()
        self.home, self.work = root / "app", root / "work"
        (self.home / "tool-workspace").mkdir(parents=True)
        self.work.mkdir()
        resources.enter_context(patch.dict(os.environ, {"MAGNITE_HOME": str(self.home)}))

    def write(self, target, content="from magnite space"):
        return prepare({"action": "write", "target": target, "content": content}, self.work)

    def reason(self, proposal):
        with self.assertRaises(Rejected) as caught:
            prepare(proposal, self.work)
        return caught.exception.reason

    def test_creates_a_file_with_exactly_the_approved_bytes(self):
        action = self.write("test.md", "a\r\n\tb")
        self.assertEqual(action.display(), {"action": "write", "target": str(self.work / "test.md"),
                                            "content": "a\r\n\tb", "replaces": False})
        self.assertFalse((self.work / "test.md").exists())
        self.assertEqual(execute(action, Event()).outcome, "completed")
        self.assertEqual((self.work / "test.md").read_bytes(), b"a\r\n\tb")

    def test_replacing_is_declared_and_keeps_permissions(self):
        target = self.work / "n.txt"
        target.write_text("old")
        target.chmod(0o600)
        action = self.write("n.txt", "")
        self.assertEqual((action.display()["replaces"], action.display()["existing_bytes"]), (True, 3))
        execute(action, Event())
        self.assertEqual(target.read_bytes(), b"")
        if os.name != "nt":
            self.assertEqual(target.stat().st_mode & 0o777, 0o600)
        self.assertEqual(sorted(p.name for p in self.work.iterdir()), ["n.txt"])

    def test_a_target_that_changed_after_approval_is_left_alone(self):
        target = self.work / "new.txt"
        created = self.write("new.txt")
        target.write_text("someone else")
        with self.assertRaises((ValueError, OSError)):
            execute(created, Event())
        self.assertEqual(target.read_text(), "someone else")
        replacing = self.write("new.txt")
        target.write_text("edited after approval")
        with self.assertRaisesRegex(ValueError, "changed"):
            execute(replacing, Event())
        self.assertEqual(target.read_text(), "edited after approval")

    def test_app_data_is_protected_except_the_tool_workspace(self):
        targets = [self.home / "blueprints.db"]
        if os.name != "nt":
            (self.work / "link").symlink_to(self.home, target_is_directory=True)
            targets.append(self.work / "link" / "models.db")
        if (self.home.parent / "APP").exists():  # case-insensitive volume
            targets.append(self.home.parent / "APP" / "models.db")
        for target in targets:
            with self.subTest(target=target):
                self.assertEqual(self.reason({"action": "write", "target": str(target), "content": "x"}),
                                 "protected_location")
        self.assertFalse(self.write(str(self.home / "tool-workspace" / "ok.txt")).display()["replaces"])

    def test_refusals_carry_a_reason(self):
        cases = [
            ({"action": "write", "target": "missing/x.txt", "content": "x"}, "folder_not_found"),
            ({"action": "write", "target": str(self.work), "content": "x"}, "not_a_file"),
            ({"action": "write", "target": "a.txt", "content": "a\x00b"}, "invalid_request"),
            ({"action": "write", "target": "a.txt", "content": "a‮b"}, "invalid_request"),
            ({"action": "write", "target": "a.txt", "content": "é" * 8001}, "invalid_request"),
            ({"action": "read", "target": "missing.md"}, "not_found"),
        ]
        for proposal, expected in cases:
            with self.subTest(expected=expected, target=proposal["target"]):
                self.assertEqual(self.reason(proposal), expected)
```

- [ ] **Step 2: Run them and watch them fail**

Run: `PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests -p "test_tool_write.py" -v`
Expected: import error, `cannot import name 'Rejected'`.

- [ ] **Step 3: Implement in `actions.py`**

- Add `Rejected`, `WriteAction(action: Literal["write"], target: Target, content: Content)` and add it to `PROPOSAL`. `Content` validates the global constraints (byte length, not character length).
- `prepare(proposal, directory) -> PreparedAction` converts every `ValueError`/`OSError` into `Rejected` with a code from the table (pydantic errors are `invalid_request`; a missing read/open path is `not_found`).
- Write branch: resolve non-strictly from `directory` (expand `~`, follow symlinks); require an existing parent directory; an existing target must be a regular file. `paths` holds the file's identity when replacing, the parent's when creating.
- Protection: walk the resolved path's parents; `samefile` with `app_home()/tool-workspace` allows, `samefile` with `app_home()` refuses. Never compare strings.
- `display()` gets its own `write` branch (today anything but open/read is shown as a command).
- `execute()`: create with mode `"xb"`; replace through a temporary file in the same folder, copy the permission bits, `os.replace`, and remove the temporary file on failure. Return `ActionResult("completed", output=f"Wrote {n} bytes to {path}.")`.
- Add `"write"` to `Approval.kind`.

- [ ] **Step 4: Run the new tests and the existing broker tests**

Run: `PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests -p "test_tool_*.py" -v`
Expected: all pass.

- [ ] **Step 5: Checkpoint** — review `git diff`; leave uncommitted.

---

### Task 2: Record rejected attempts

**Files:**
- Modify: `backend/src/backend/tooling/approvals.py`, `backend/src/backend/tooling/actions.py`
- Test: `backend/tests/test_tool_approvals.py`

**Interfaces:**
- Consumes: `Rejected` (Task 1).
- Produces: `actions.describe(request: dict) -> tuple[str, dict]`, accepting a broker action (`{"action": ...}`) or a model call (`{"name": ..., "arguments": ...}`), returning a kind in `read, open, command, write, unknown` and a display dict (`action` plus `target`, or `executable` and `arguments`; strings cut to 4096; never file content). A model command call shows its command line as `target`; an unknown tool shows its name as `target`.
- Produces: `ToolBroker.reject(run_id: str, node_id: str, kind: str, action: dict, reason: str) -> None`; `ToolBroker.list()` returns these merged by `created_at` with status `"rejected"`, `outcome=reason`, `fingerprint=""`. Keep at most `MAX_REJECTED_RUNS = 50` runs.

- [ ] **Step 1: Write the failing test** (in `ToolApprovalTests`)

```python
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
```

Also update `test_denial_expiry_cancellation_and_malformed_actions_never_dispatch`: for `malformed`, replace `self.assertEqual(audit, [])` with `self.assertEqual([r["status"] for r in audit], ["rejected"])`.

- [ ] **Step 2: Run** — expected: `IndexError` on the empty list.

- [ ] **Step 3: Implement.** Add `"rejected"` to `ApprovalStatus` and `"unknown"` to `kind`. `perform()` catches `Rejected` from `actions.prepare`, calls `self.reject(run_id, node_id, *actions.describe(proposal), exc.reason)` and re-raises. Rejected records are held in a dict under `self._condition`; they write no database row and no event, and `decide()` already returns 404 for them.

- [ ] **Step 4: Run** `-p "test_tool_*.py"` — all pass.

- [ ] **Step 5: Checkpoint.**

---

### Task 3: `write_file`, shell syntax and rejection reporting in the bridge

**Files:**
- Modify: `backend/src/backend/tooling/pi_bridge.py`, `backend/src/backend/blueprints/runner.py:340`
- Test: `backend/tests/test_tool_approvals.py`

**Interfaces:**
- Consumes: `Rejected`, `actions.describe`, `ToolBroker.reject`.
- Produces: `pi_bridge.run(..., reject: Callable[[str, dict, str], None] | None = None)` called as `reject(kind, action, reason)` only for failures **before** `approve` is called (`approve` records its own). `receipt_text({"status": "rejected", "outcome": code})` says the action was not run and why; for `not_found` it adds "To create it, call write_file."

- [ ] **Step 1: Write the failing tests.** In `ReplyParsingTests` (import `Rejected`):

```python
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

    def test_a_leading_tilde_is_expanded_and_shown(self):
        action = pi_bridge.proposal({"name": "run_command", "arguments": {"command": "touch ~/x.txt a~b"}})
        self.assertEqual(action["arguments"], [os.path.expanduser("~/x.txt"), "a~b"])

    def test_a_missing_file_receipt_names_the_reason_and_the_way_forward(self):
        text = pi_bridge.receipt_text({"status": "rejected", "outcome": "not_found"})
        self.assertIn("not run", text)
        self.assertIn("does not exist", text)
        self.assertIn("write_file", text)
```

In `test_tool_calls_become_exact_broker_actions`, delete the `ls | wc` case. In `PiBridgeTests`:

```python
    def test_a_refusal_before_approval_is_reported_to_the_caller(self):
        replies = iter(['{"name":"run_command","arguments":{"command":"ls | wc"}}', "Done."])
        refused = []
        with patch("backend.tooling.pi_bridge._generate", side_effect=lambda *args: next(replies)):
            pi_bridge.run("List", object(), lambda proposal: self.fail("never proposed"), Event(),
                          reject=lambda *args: refused.append(args))
        self.assertEqual([(kind, reason) for kind, _, reason in refused], [("command", "shell_syntax")])
```

In `test_denials_continue_until_the_action_budget_and_then_answer`, pass `reject=` and assert it received exactly one `("open", ..., "action_limit")`.

- [ ] **Step 2: Run** — expected: `Unknown tool` for `write_file`, and `reject` is an unexpected argument.

- [ ] **Step 3: Implement**

- Add the `write_file` entry to `TOOLS` after `read_file`: description "Create a text file, or replace its contents.", required `path` ("File path, relative to the working folder or absolute") and `content` ("The full text to put in the file").
- `_call`: map a `target` argument to `path` for `write_file`.
- `system_prompt`: insert "- To create or change a file, call write_file." before the `run_command` line. `_JSON_PROTOCOL`: add `{"name":"write_file","arguments":{"path":"notes.txt","content":"text"}}`.
- `proposal()`: handle `write_file`; for `run_command` first check `_has_shell_operator(command: str) -> bool` — scan characters tracking single and double quotes and (except on Windows) backslash escapes; any of `| < > & ;` outside them is an operator. Then expand arguments equal to `~` or starting `~/` (also `~\` on Windows).
- `run()`: if `proposal()` raises, call `reject(*actions.describe(call), reason)`; otherwise call `approve` and map `Rejected` to `{"status": "rejected", "outcome": exc.reason}` without reporting again. Report `action_limit` the same way as a `proposal()` failure.
- `runner._run_tool`: pass `reject=lambda kind, action, reason: self.broker.reject(job.run.id, node.id, kind, action, reason)`.

- [ ] **Step 4: Run** `-p "test_tool_*.py"` with `MAGNITE_REQUIRE_PI=1` — all pass, none skipped.

- [ ] **Step 5: Checkpoint.**

---

### Task 4: Unknown and incomplete tool calls

**Files:**
- Modify: `backend/src/backend/tooling/pi_bridge.py`
- Test: `backend/tests/test_tool_approvals.py`

**Interfaces:**
- Produces: `call_problem(raw: str, offered: bool) -> tuple[str, str] | None` returning `(reason, tool_name)` for `unknown_tool`, `incomplete_call`, or (`offered=False`) `action_limit`. `parse_reply` is unchanged.

- [ ] **Step 1: Write the failing tests**

```python
    def test_unknown_and_incomplete_calls_are_named(self):
        cases = [
            ('<tool_call>{"name":"delete_all","arguments":{}}</tool_call>', True, ("unknown_tool", "delete_all")),
            ('{"name":"delete_all","arguments":{}}', True, ("unknown_tool", "delete_all")),
            ('{"name":"list_folder","arguments":{}', True, ("incomplete_call", "")),
            ('<tool_call>\n{"name": "read_', True, ("incomplete_call", "")),
            ('{"name":"read_file","arguments":{"path":"a"}}', False, ("action_limit", "read_file")),
            ('{"name":"Alice","age":3}', True, None),
            ('{"answer":"hi"}', True, None),
            ("Paris is the capital.", True, None),
        ]
        for raw, offered, expected in cases:
            with self.subTest(raw=raw):
                self.assertEqual(pi_bridge.call_problem(raw, offered), expected)
```

In `PiBridgeTests`:

```python
    def test_an_unknown_tool_gets_a_plain_message_and_a_record(self):
        refused = []
        with patch("backend.tooling.pi_bridge._generate",
                   return_value='<tool_call>{"name":"delete_all","arguments":{}}</tool_call>'):
            result = pi_bridge.run("Clean up", object(), lambda p: self.fail("never"), Event(),
                                   reject=lambda *args: refused.append(args))
        self.assertEqual(result, "Nothing ran: the model asked for a tool Magnite does not have (delete_all).")
        self.assertEqual([(kind, reason) for kind, _, reason in refused], [("unknown", "unknown_tool")])
```

- [ ] **Step 2: Run** — expected: no attribute `call_problem`.

- [ ] **Step 3: Implement.** A JSON object counts as a call only if it has a string `name` and an `arguments` or `parameters` key. In `run()`'s `generate` case, after the existing cut-off check, when `approve` is not None and there are no calls, replace the text and call `reject` (kind from `describe`, `unknown` for incomplete). Exact messages: "Nothing ran: the model asked for a tool Magnite does not have ({name})." / "Nothing ran: the model's tool call was incomplete." / "Nothing ran: the action limit for this task was reached."

- [ ] **Step 4: Run** `-p "test_tool_*.py"` — all pass, including the unchanged `parse_reply` cases.

- [ ] **Step 5: Checkpoint.**

---

### Task 5: Reminders

**Files:**
- Modify: `backend/src/backend/tooling/pi_bridge.py`
- Test: `backend/tests/test_tool_approvals.py`

**Interfaces:**
- Produces: `REQUEST_REMINDER = "(Call a tool for this if it needs my computer.)"`, `STEP_REMINDER = "(If my request has more steps, call the next tool now.)"`.

- [ ] **Step 1: Write the failing test**

```python
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
```

- [ ] **Step 2: Run** — expected: no attribute `STEP_REMINDER`.

- [ ] **Step 3: Implement.** Only when `approve` is not None: append the request reminder to the last user message of `history` (after validation), and the step reminder to a receipt whose status is `completed` while `proposals < MAX_PROPOSALS`. Strip both strings from the final answer.

- [ ] **Step 4: Run** `-p "test_tool_*.py"` — all pass; `test_text_agent_passes_instructions...` stays unchanged.

- [ ] **Step 5: Checkpoint.**

---

### Task 6: llama.cpp context and output trimming

**Files:**
- Modify: `backend/src/backend/inference/engines/llamacpp.py:28-89`, `backend/src/backend/tooling/pi_bridge.py` (`_generate`)
- Create: `backend/tests/test_tool_context.py`

**Interfaces:**
- Produces: `LlamaCppEngine(n_ctx=8192)` by default; after `load()`, `context_size()` is `min(n_ctx, self._llm._model.n_ctx_train())`. `pi_bridge._fit_tool_output(messages: list[dict], size: Callable[[list[dict]], int], limit: int) -> list[dict]` returns a copy that fits, or raises the existing "longer than the loaded model's context" `ValueError`.

- [ ] **Step 1: Write the failing tests**

```python
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from backend.inference.engines.llamacpp import LlamaCppEngine
from backend.tooling import pi_bridge


class ToolContextTests(unittest.TestCase):
    def test_llamacpp_context_is_8192_capped_by_the_model(self):
        for trained, expected in ((40960, 8192), (4096, 4096)):
            llama = lambda **options: SimpleNamespace(_model=SimpleNamespace(n_ctx_train=lambda: trained))
            with self.subTest(trained=trained), \
                    patch.dict(sys.modules, {"llama_cpp": SimpleNamespace(Llama=llama)}), \
                    patch.object(LlamaCppEngine, "_resolve_gguf_path", return_value="m.gguf"):
                engine = LlamaCppEngine("m")
                engine.load()
                self.assertEqual(engine.context_size(), expected)

    def test_oversized_tool_output_is_trimmed_to_fit(self):
        size = lambda items: sum(len(item["content"]) for item in items)
        messages = [{"role": "user", "content": "q"}, {"role": "tool", "content": "x" * 10_000}]
        fitted = pi_bridge._fit_tool_output(messages, size, 2_000)
        self.assertLessEqual(size(fitted), 2_000)
        self.assertIn("truncated to fit", fitted[1]["content"])
        self.assertEqual(len(messages[1]["content"]), 10_000)
        with self.assertRaisesRegex(ValueError, "longer than"):
            pi_bridge._fit_tool_output([{"role": "user", "content": "x" * 3_000}], size, 2_000)
```

- [ ] **Step 2: Run** `-p "test_tool_context.py"` — expected: 2048, and no `_fit_tool_output`.

- [ ] **Step 3: Implement.** Halve the longest `tool` message over 400 characters, ending it "\n[... truncated to fit the model's context ...]", until `size <= limit`. In `_generate`, use it with `limit = engine.context_size() - TURN_TOKENS` on both the native and plain paths, measuring the rendered prompt as today.

- [ ] **Step 4: Run the whole backend suite**

Run: `PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests -v`
Expected: OK.

- [ ] **Step 5: Checkpoint.**

---

### Task 7: Client cards, caption and hint

**Files:**
- Modify: `app/src/panels/blueprintbackend.cpp` (`displayRun` ~line 284, the kept-details loop ~line 586)
- Modify: `app/src/panels/blueprintpanel.cpp` (`statusTone`/`statusLabel` ~157-180, `approvalView` ~192, welcome text ~3120, tool result ~3185)
- Modify: `app/src/panels/blueprintpreview.cpp` (`statusLabel` ~941)
- Test: `app/src/testing/blueprint_tests.cpp` (`runLiveBlueprintChecks`)

**Interfaces:**
- Consumes: approvals with `kind` `write`/`unknown`, status `rejected`, `outcome` = reason code, `action.content/replaces/existing_bytes`.
- Produces: decision fields `content`, `replaces`, `existingBytes`.

- [ ] **Step 1: Write the failing checks** after the "Approve once completes the exact command" block, driving the Blueprint approval node the same way (no model needed). Use a `QTemporaryDir scratch`.
  - Submit `{"request":{"action":"write","target":scratch/approved.txt,"content":"from magnite space"}}`. Check: `visibleText(panel, "Create file")`, a visible `QPlainTextEdit` named `blueprintCode` whose text equals the content, the file does not exist yet; after "Approve once" the run passes and the file holds exactly the content. Message: "a write shows its content and happens only after approval".
  - Submit it again with content `"second"`. Check: `visibleText(panel, "Replace file")` and `visibleText(panel, "This replaces the current contents")`; "Deny" leaves the file unchanged.
  - Submit `{"request":{"action":"read","target":scratch/missing.txt}}`. Check: `visibleText(panel, "Not run")` and `visibleText(panel, "The file does not exist.")`.
  - In the `testModel` branch, after the "Reply with the word ready" turn, check `visibleText(chat, "No actions ran")`.

- [ ] **Step 2: Build and run; the new checks fail**

Run: both build commands, then `MAGNITE_BLUEPRINTS_LIVE_CHECK=1 QT_QPA_PLATFORM=offscreen ./build/magnite-app --ui-check`
Expected: FAIL on the three new checks.

- [ ] **Step 3: Implement**

- Type labels: `command` "Run command", `read` "Read file or folder", `open` "Open target", `write` "Replace file" / "Create file" (from `replaces`; "Write file" when details are gone), `unknown` "Tool request". Never fall through to "Open target".
- Keep `content`, `replaces`, `existingBytes` and `type` in the reviewed-details loop.
- `write` card: "Path" line; when replacing, "This replaces the current contents (%1)." with `QLocale().formattedDataSize`; content in a read-only `QPlainTextEdit`, object name `blueprintCode`, `NoWrap`, maximum height 200. Icon `file-text`.
- `rejected`: status label "Not run", warning tone and style, details visible without the toggle, and the reason text from the table (shown whether or not the preview is live).
- Caption: when a passed tool run has `details` and no decision with status `completed` and none with outcome `nonzero_exit` or `timed_out`, the model line reads "{model} · No actions ran". (A command that ran and failed did run.)
- Welcome text: append " Very small models often miss steps. Qwen3 1.7B or larger is recommended."

- [ ] **Step 4: Build and verify**

Run: the live check again, then `./build/magnite-app --theme-check` and `QT_QPA_PLATFORM=offscreen ./build/magnite-app --ui-check`
Expected: 0 failures. Capture the three cards in light and dark and look at them.

- [ ] **Step 5: Checkpoint.**

---

### Task 8: Selftest, docs and verification

**Files:**
- Modify: `app/src/mainwindow.cpp` (`toolModeSelfTest`, ~line 1730)
- Modify: `docs/BLUEPRINTS.md`, `docs/LOCAL_API.md`, `docs/STORAGE_AND_PRIVACY.md`

- [ ] **Step 1: Extend the selftest** with two more `runTask` calls: "Create a file at {tmp}/magnite-selftest-write.txt containing: purple walrus", once with "Deny" (file must not exist) and once with "Approve once" (file must exist and contain "purple walrus"). Remove the file before the first.

- [ ] **Step 2: Update the docs**
  - `BLUEPRINTS.md`, "Tool requests and approvals": writing files and its limits (existing folder, about 2–3 KB per reply, app data protected), "Not run" cards, "No actions ran", shell syntax.
  - `LOCAL_API.md`, "Tool chat and approvals": `write` action fields; `kind` `write`/`unknown`; status `rejected` with reason codes, held in memory.
  - `STORAGE_AND_PRIVACY.md`: file content to be written is shown while pending and kept by the app until it closes; the audit on disk stores neither paths nor content.

- [ ] **Step 3: Full verification**

Run: backend suite; `PYTHONPATH=backend/src MAGNITE_REQUIRE_PI=1 .venv/bin/python -m unittest discover -s packaging/tests -v`; `--theme-check`; `./build/magnite-app --selftest`.
Expected: all pass.

- [ ] **Step 4: Ask the user, then run the real-model checks**
  - `MAGNITE_SELFTEST_TOOLMODE=1 ./build/magnite-app --selftest` loads torch (3–4 GB on this Mac), so ask first. Expected: `[selftest] PASS (0 failures)`.
  - Re-run the 2026-11-26 probes (`~/.claude/projects/-Users-architbhuta-magnite-llm/tool-mode-probes/`: the screenshot replay, one-step, three-step and history sets) on 0.6B and 1.7B GGUF against the built code, with no monkeypatched tool, prompt line or reminder (`approve` now receives `target`, not `path`).
  - Expected: no worse than the design's figures; no extra calls on one-step requests; no answer containing markup or a reminder; a 6 KB file read succeeds on llama.cpp.

- [ ] **Step 5: Report to the user** what passed and what did not, then walk through what was built in plain language.
