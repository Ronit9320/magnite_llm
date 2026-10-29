"""macOS has no PR_SET_PDEATHSIG; the backend watches its parent instead (ISSUE-001)."""
import os
import signal
import subprocess
import sys
import time
import unittest
from unittest.mock import patch

from backend import main


class ParentWatchTests(unittest.TestCase):
    def test_signals_self_once_reparented(self):
        ppids = iter([4242, 4242, 1])
        with patch.object(main.os, "getppid", side_effect=lambda: next(ppids)), \
                patch.object(main.os, "kill") as kill, patch.object(main.time, "sleep"), \
                patch.object(main.os, "_exit") as hard_exit:
            main._watch_parent(4242, interval=0)
        kill.assert_called_once_with(os.getpid(), signal.SIGTERM)
        hard_exit.assert_called_once()

    def test_darwin_starts_watcher_only_with_a_real_parent(self):
        for ppid, expected in ((4242, 1), (1, 0)):
            with patch.object(main.sys, "platform", "darwin"), \
                    patch.object(main.os, "getppid", return_value=ppid), \
                    patch.object(main.threading, "Thread") as thread:
                main._enable_parent_death_signal()
            self.assertEqual(thread.call_count, expected, f"ppid={ppid}")

    @unittest.skipUnless(sys.platform == "darwin", "exercises the macOS watcher for real")
    def test_backend_exits_when_parent_is_killed(self):
        child = ("import time; from backend import main; main._PARENT_POLL_SECONDS = 0.2; "
                 "main._enable_parent_death_signal(); print('ready', flush=True); time.sleep(60)")
        parent = ("import subprocess, sys, time; "
                  f"p = subprocess.Popen([sys.executable, '-c', {child!r}], stdout=subprocess.PIPE, text=True); "
                  "p.stdout.readline(); print(p.pid, flush=True); time.sleep(60)")
        proc = subprocess.Popen([sys.executable, "-c", parent], stdout=subprocess.PIPE, text=True,
                                env=dict(os.environ))
        try:
            child_pid = int(proc.stdout.readline())
            os.kill(proc.pid, signal.SIGKILL)
            proc.wait(5)
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                try:
                    os.kill(child_pid, 0)
                except ProcessLookupError:
                    break
                time.sleep(0.1)
            else:
                os.kill(child_pid, signal.SIGKILL)
                self.fail("backend child survived its parent's SIGKILL")
        finally:
            if proc.poll() is None:
                proc.kill()
            proc.wait(5)
            proc.stdout.close()


if __name__ == "__main__":
    unittest.main()
