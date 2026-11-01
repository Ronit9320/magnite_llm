"""Child process that downloads one Hugging Face repo and reports byte progress.

The backend runs this separately so a cancel can simply kill it. Python-level
callbacks cannot interrupt a native (Xet) transfer, and the Hub client swallows
exceptions raised inside its progress callbacks, so killing the process is the
only cancel that takes effect immediately.

Protocol, one JSON object per stdout line (anything else is ignored):

    {"total": N}                  expected bytes, from repo metadata
    {"done": D, "total": T}       progress so far
    {"result": "ok", "path": P}   finished; P is the snapshot directory
    {"error": "..."}              failed

Usage: python -m backend.models.pull_worker <repo_id> <cache_dir>
"""
from __future__ import annotations

import json
import os
import sys
import threading
import time

from huggingface_hub import HfApi, snapshot_download
from tqdm import tqdm

_PARENT_POLL_SECONDS = 1.0
_null_sink = None


def _null():
    """A shared write sink so progress bars track state without drawing anything."""
    global _null_sink
    if _null_sink is None:
        _null_sink = open(os.devnull, "w")
    return _null_sink


class Reporter:
    """Writes protocol lines. Progress is throttled; the latest value is never lost."""

    def __init__(self, out, interval: float = 0.25):
        self._out = out
        self._interval = interval
        self._lock = threading.Lock()
        self._last_emit: float | None = None
        self._pending: dict | None = None
        self._sent: dict | None = None
        self._expected = 0

    def _write(self, payload: dict) -> None:
        try:
            self._out.write(json.dumps(payload) + "\n")
            self._out.flush()
        except (OSError, ValueError):
            pass   # the backend went away; the parent watch ends this process

    def message(self, payload: dict) -> None:
        with self._lock:
            self._write(payload)

    def expect(self, total: int) -> None:
        """Announce the expected size (from repo metadata)."""
        with self._lock:
            self._expected = max(self._expected, total)
            self._write({"total": self._expected})

    def update(self, done: int, total: int) -> None:
        with self._lock:
            self._pending = {"done": int(done), "total": int(max(total, self._expected))}
            now = time.monotonic()
            if self._last_emit is None or now - self._last_emit >= self._interval:
                self._emit_pending(now)

    def flush(self) -> None:
        with self._lock:
            if self._pending is not None and self._pending != self._sent:
                self._emit_pending(time.monotonic())

    def _emit_pending(self, now: float) -> None:
        self._write(self._pending)
        self._sent = self._pending
        self._last_emit = now


def _progress_bar_class(reporter: Reporter):
    """tqdm subclass feeding the reporter.

    The Hub client uses one shared byte bar whose total grows as files are
    discovered, plus a file-count bar and (for Xet files) a second byte bar that
    counts the same bytes. Only byte bars count, and progress is the largest
    absolute value among them, so nothing is double-counted.
    """
    bars: list = []
    guard = threading.Lock()

    class ProgressBar(tqdm):
        def __init__(self, *args, **kwargs):
            kwargs.pop("file", None)
            kwargs["disable"] = False   # a disabled bar would not track its position
            super().__init__(*args, file=_null(), **kwargs)
            if self.unit == "B":
                with guard:
                    bars.append(self)

        def update(self, n=1):
            result = super().update(n)
            if self.unit == "B":
                with guard:
                    done = max(int(b.n) for b in bars)
                    total = max(int(b.total or 0) for b in bars)
                reporter.update(done, total)
            return result

    return ProgressBar


def _expected_bytes(repo_id: str) -> int:
    """Total repo size from metadata; 0 if unavailable (progress then uses the bar's total)."""
    try:
        info = HfApi().model_info(repo_id, files_metadata=True)
        return sum(int(s.size or 0) for s in (info.siblings or []))
    except Exception:  # noqa: BLE001 -- metadata is a nicety, never a reason to fail
        return 0


def _start_parent_watch(interval: float = _PARENT_POLL_SECONDS) -> None:
    """Exit if the backend dies, so a killed backend cannot leave a download running."""
    parent = os.getppid()
    if parent == 1:
        return

    def watch():
        while os.getppid() == parent:
            time.sleep(interval)
        os._exit(3)

    threading.Thread(target=watch, name="parent-watch", daemon=True).start()


def main(argv: list[str] | None = None, out=None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    reporter = Reporter(out if out is not None else sys.stdout)
    if len(argv) != 2:
        reporter.message({"error": "usage: pull_worker <repo_id> <cache_dir>"})
        return 2
    repo_id, cache_dir = argv
    _start_parent_watch()
    try:
        total = _expected_bytes(repo_id)
        if total:
            reporter.expect(total)
        path = snapshot_download(
            repo_id=repo_id, cache_dir=cache_dir,
            tqdm_class=_progress_bar_class(reporter), local_files_only=False)
        reporter.flush()
        reporter.message({"result": "ok", "path": str(path)})
        return 0
    except Exception as exc:  # noqa: BLE001
        reporter.flush()
        reporter.message({"error": str(exc) or type(exc).__name__})
        return 1


if __name__ == "__main__":
    sys.exit(main())
