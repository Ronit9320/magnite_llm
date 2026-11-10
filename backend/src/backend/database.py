"""Short-lived SQLite transactions which release OS file handles deterministically."""
from contextlib import contextmanager
import os
import sqlite3
import threading
import time
from typing import Callable

# Human-readable startup repairs, surfaced by GET /health so the desktop app can tell the user.
# File names only, never full paths: /health is reachable without a token.
NOTICES: list[str] = []
_notice_lock = threading.Lock()


def add_notice(text: str) -> None:
    with _notice_lock:
        NOTICES.append(text)


@contextmanager
def connection(path):
    conn = sqlite3.connect(path)
    conn.row_factory = sqlite3.Row
    try:
        with conn:
            yield conn
    finally:
        # sqlite3.Connection.__exit__ commits/rolls back but does not close.
        # Windows cannot remove/replace an open database file.
        conn.close()


_SIDECARS = ("-wal", "-shm", "-journal")


def _quarantine(path: str) -> str:
    """Move a corrupt database and its journal files aside; return the new name."""
    stamp = time.strftime("%Y%m%dT%H%M%S", time.gmtime())
    target, n = f"{path}.corrupt-{stamp}", 1
    while os.path.exists(target):
        n += 1
        target = f"{path}.corrupt-{stamp}-{n}"
    os.replace(path, target)
    for suffix in _SIDECARS:
        if os.path.exists(path + suffix):
            os.replace(path + suffix, target + suffix)
    return target


def open_or_quarantine(path: str, init: Callable[[sqlite3.Connection], None], hint: str = "") -> None:
    """Run ``init(conn)`` against the database at ``path``.

    If the file is not a usable SQLite database (garbage, truncated, malformed) it is moved
    aside to ``<path>.corrupt-<UTC stamp>`` with its journal files, ``init`` runs once more on a
    fresh file, and a notice is recorded. ``OperationalError`` (locked, disk full, cannot open)
    is *not* corruption: it propagates and the file is left alone.
    """
    try:
        with connection(path) as conn:
            init(conn)
        return
    except sqlite3.OperationalError:
        raise
    except sqlite3.DatabaseError:
        pass
    name = os.path.basename(path)
    aside = os.path.basename(_quarantine(path))
    add_notice(f"{name} could not be read, so it was set aside as {aside} and a new one was created. {hint}".strip())
    with connection(path) as conn:
        init(conn)
