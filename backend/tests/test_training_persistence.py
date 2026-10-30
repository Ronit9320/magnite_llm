from contextlib import contextmanager
import json
from pathlib import Path
import tempfile
import threading
import unittest
from unittest.mock import MagicMock, patch

from backend.database import connection
from backend.training.jobs import TrainingJobManager
from backend.training.schemas import TrainingJob


class TrainingPersistenceTests(unittest.TestCase):
    def test_older_shutdown_snapshot_cannot_overwrite_completed_history(self):
        with tempfile.TemporaryDirectory() as directory:
            manager = TrainingJobManager(directory, MagicMock(), MagicMock())
            job = TrainingJob(id="job", name="job", status="running", base_model="test", dataset_id="test")
            old_snapshot = threading.Event()
            release_old = threading.Event()
            done_finished = threading.Event()
            errors = []

            @contextmanager
            def delayed_connection(path):
                with connection(path) as conn:
                    class Proxy:
                        def execute(self, sql, params):
                            if json.loads(params[1])["status"] == "running":
                                old_snapshot.set()
                                if not release_old.wait(10):
                                    raise TimeoutError("stale snapshot was not released")
                            return conn.execute(sql, params)
                    yield Proxy()

            def persist(completed=False):
                try:
                    manager._persist(job)
                    if completed:
                        done_finished.set()
                except BaseException as exc:
                    errors.append(exc)

            threads = []
            try:
                with patch("backend.training.jobs.connection", side_effect=delayed_connection):
                    old = threading.Thread(target=persist)
                    threads.append(old)
                    old.start()
                    self.assertTrue(old_snapshot.wait(5))
                    job.status = "done"
                    completed = threading.Thread(target=persist, args=(True,))
                    threads.append(completed)
                    completed.start()
                    # Without serialization the done commit can finish first,
                    # after which releasing the stale write loses completion.
                    done_finished.wait(1)
                    release_old.set()
                    for thread in threads:
                        thread.join(10)
                        self.assertFalse(thread.is_alive())
                self.assertEqual(errors, [])
                with connection(Path(directory) / "training_jobs.db") as conn:
                    saved = conn.execute("SELECT record FROM jobs WHERE id='job'").fetchone()[0]
                self.assertEqual(json.loads(saved)["status"], "done")
            finally:
                release_old.set()
                for thread in threads:
                    thread.join(10)
                manager.shutdown()
