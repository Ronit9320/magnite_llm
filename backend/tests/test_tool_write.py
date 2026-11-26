"""The approved file-write action and the reasons given for refused actions."""

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

    def test_a_path_cannot_disguise_its_name_with_direction_overrides(self):
        for proposal in ({"action": "write", "target": "invoice\u202etxt.sh", "content": "x"},
                         {"action": "read", "target": "a\u2066.txt"},
                         {"action": "command", "executable": "ls", "arguments": ["a\u202eb"]}):
            with self.subTest(action=proposal["action"]):
                self.assertEqual(self.reason(proposal), "invalid_request")

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


if __name__ == "__main__":
    unittest.main()
