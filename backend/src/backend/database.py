"""Short-lived SQLite transactions which release OS file handles deterministically."""
from contextlib import contextmanager
import sqlite3


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
