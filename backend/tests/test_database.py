from pathlib import Path
import sqlite3
import tempfile
import unittest

from backend.database import connection


class DatabaseLifetimeTests(unittest.TestCase):
    def test_commit_rollback_and_close_do_not_depend_on_garbage_collection(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "store.db"
            with connection(path) as first:
                first.execute("CREATE TABLE items (value TEXT)")
                first.execute("INSERT INTO items VALUES ('kept')")
            with self.assertRaises(sqlite3.ProgrammingError):
                first.execute("SELECT * FROM items")
            with self.assertRaisesRegex(RuntimeError, "cancelled"):
                with connection(path) as failed:
                    failed.execute("INSERT INTO items VALUES ('rolled back')")
                    raise RuntimeError("cancelled")
            with self.assertRaises(sqlite3.ProgrammingError):
                failed.execute("SELECT * FROM items")
            with connection(path) as final:
                self.assertEqual([row[0] for row in final.execute("SELECT value FROM items")], ["kept"])
            # Keep references to all connections alive while exercising file
            # replacement/removal. This fails on Windows if a handle leaked.
            moved = path.with_name("moved.db")
            path.rename(moved)
            moved.unlink()
