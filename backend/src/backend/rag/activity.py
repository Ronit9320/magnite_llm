"""One heavy RAG operation at a time, with progress.

Ingest and reindex both load an embedding model and hold large buffers, so they run one after
another. Waiting requests queue on a lock they release if cancelled (no stuck request), and the
running operation publishes its phase and progress for ``GET /rag/activity``.
"""
from __future__ import annotations

import threading
from contextlib import contextmanager
from typing import Optional

from backend.rag.batching import IngestCancelled


class Activity:
    def __init__(self):
        self._lock = threading.Lock()
        self._state = self._idle()
        self._waiting = 0

    @staticmethod
    def _idle() -> dict:
        return {"active": False, "kind": None, "name": "", "phase": "", "done": 0, "total": 0}

    def start(self, kind: str, name: str, total: int = 0) -> None:
        with self._lock:
            self._state = {"active": True, "kind": kind, "name": name, "phase": "starting",
                           "done": 0, "total": total}

    def set_phase(self, phase: str, total: Optional[int] = None) -> None:
        with self._lock:
            self._state["phase"] = phase
            if total is not None:
                self._state["total"] = total
                self._state["done"] = 0

    def advance(self, done: int) -> None:
        with self._lock:
            self._state["done"] = done

    def finish(self) -> None:
        with self._lock:
            self._state = self._idle()

    def _wait_delta(self, delta: int) -> None:
        with self._lock:
            self._waiting += delta

    def snapshot(self) -> dict:
        with self._lock:
            return {**self._state, "waiting": self._waiting}


_activity = Activity()
_work_lock = threading.Lock()


def get_activity() -> Activity:
    return _activity


@contextmanager
def exclusive(kind: str, name: str, cancelled: Optional[threading.Event] = None, poll: float = 0.1):
    """Run one operation at a time. A request cancelled while queued (or before it starts) raises
    ``IngestCancelled`` without ever taking the lock."""
    _activity._wait_delta(1)
    try:
        while True:
            if cancelled is not None and cancelled.is_set():
                raise IngestCancelled("Operation cancelled before it started")
            if _work_lock.acquire(timeout=poll):
                break
    finally:
        _activity._wait_delta(-1)
    try:
        _activity.start(kind, name)
        yield _activity
    finally:
        _activity.finish()
        _work_lock.release()
