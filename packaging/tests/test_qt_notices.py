import subprocess
import sys
import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from macos import qt_notices


class QtNoticeDownloadTests(unittest.TestCase):
    def test_api_authentication_uses_stdin_and_never_the_command_line(self):
        token = "test-ci-token"
        with patch.dict("os.environ", {"GH_TOKEN": token}), patch.object(
            qt_notices.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, b'{"tree": []}', b"")
        ) as run:
            self.assertEqual(qt_notices.fetch("https://api.github.com/repos/qt/qtbase/git/trees/v6.8.3"), b'{"tree": []}')
        command = run.call_args.args[0]
        self.assertNotIn(token, " ".join(command))
        self.assertIn("@-", command)
        self.assertEqual(run.call_args.kwargs["input"], f"Authorization: Bearer {token}\n".encode())

    def test_token_is_not_sent_to_raw_files_http_or_lookalike_hosts(self):
        for url in (
            "https://raw.githubusercontent.com/qt/qtbase/v6.8.3/LICENSES/LGPL-3.0-only.txt",
            "http://api.github.com/repos/qt/qtbase",
            "https://api.github.com.example.com/repos/qt/qtbase",
            "https://api.github.com@other.example/repos/qt/qtbase",
        ):
            with self.subTest(url=url), patch.dict("os.environ", {"GH_TOKEN": "test-ci-token"}), patch.object(
                qt_notices.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, b"license", b"")
            ) as run:
                self.assertEqual(qt_notices.fetch(url), b"license")
                self.assertEqual(run.call_args.kwargs["input"], b"")
                self.assertNotIn("--header", run.call_args.args[0])

    def test_local_tokenless_download_still_works(self):
        with patch.dict("os.environ", {"GH_TOKEN": ""}), patch.object(
            qt_notices.subprocess, "run", return_value=subprocess.CompletedProcess([], 0, b"metadata", b"")
        ) as run:
            self.assertEqual(qt_notices.fetch("https://api.github.com/repos/qt/qtbase"), b"metadata")
            self.assertEqual(run.call_args.kwargs["input"], b"")

    def test_http_failure_reports_the_reason_without_caching_partial_data_or_token(self):
        token = "test-ci-token"
        stderr = f"curl: (22) The requested URL returned error: 403 {token}".encode()
        with tempfile.TemporaryDirectory() as temporary, patch.dict("os.environ", {"GH_TOKEN": token}), patch.object(
            qt_notices.subprocess, "run", return_value=subprocess.CompletedProcess([], 22, b"partial", stderr)
        ):
            cache = Path(temporary)
            with self.assertRaises(RuntimeError) as raised:
                list(qt_notices.attributions(cache))
            self.assertIn("403", str(raised.exception))
            self.assertIn("qtbase", str(raised.exception))
            self.assertNotIn(token, str(raised.exception))
            self.assertFalse((cache / "qtbase-tree.json").exists())


if __name__ == "__main__":
    unittest.main()
