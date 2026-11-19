"""Profile statistics: Blueprint run counts and the /system/stats aggregate."""
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from fastapi.testclient import TestClient

from backend.blueprints.store import BlueprintStore
from backend.database import connection
from backend.routes import system
from backend.server import app


class BlueprintCounts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.store = BlueprintStore(str(Path(self.temp.name) / "blueprints.db"))

    def add_run(self, run_id, status, tool=False, created="2026-11-01T10:00:00+00:00"):
        summary = {"id": run_id, "status": status, "tool_chat": tool}
        with connection(self.store.path) as conn:
            conn.execute("INSERT INTO runs (id, blueprint_id, status, created_at, summary, snapshot) VALUES (?, ?, ?, ?, ?, '{}')",
                         (run_id, "bp", status, created, json.dumps(summary)))

    def test_empty_store(self):
        self.assertEqual(self.store.counts(),
                         {"blueprints": 0, "runs": 0, "tool_tasks": 0, "passed": 0, "first": None})

    def test_counts_workflow_runs_and_tool_tasks_separately(self):
        with connection(self.store.path) as conn:
            for revision in (1, 2):
                conn.execute("INSERT INTO revisions VALUES ('a', ?, 'now', 'A', '{}')", (revision,))
            conn.execute("INSERT INTO revisions VALUES ('b', 1, 'now', 'B', '{}')")
        self.add_run("r1", "passed", created="2026-10-02T09:00:00+00:00")
        self.add_run("r2", "failed")
        self.add_run("t1", "passed", tool=True)
        counts = self.store.counts()
        self.assertEqual(counts["blueprints"], 2)
        self.assertEqual(counts["runs"], 2)
        self.assertEqual(counts["tool_tasks"], 1)
        self.assertEqual(counts["passed"], 2)
        self.assertTrue(counts["first"].startswith("2026-10-02"))


class StatsRoute(unittest.TestCase):
    def test_unavailable_sections_are_null_not_errors(self):
        response = TestClient(app).get("/system/stats")
        self.assertEqual(response.status_code, 200)
        body = response.json()
        for name in ("models", "training", "adapters", "datasets", "knowledge", "blueprints"):
            self.assertIn(name, body)
        self.assertIn("since", body)

    def test_since_is_the_earliest_stamp(self):
        self.assertEqual(system._earliest(["2026-11-03", "", None, "2026-10-01T00:00:00Z"]), "2026-10-01T00:00:00Z")
        self.assertIsNone(system._earliest([None, ""]))

    def test_one_failing_store_keeps_the_others(self):
        with patch("backend.routes.adapters._get_store", side_effect=RuntimeError("down")):
            body = TestClient(app).get("/system/stats").json()
        self.assertIsNone(body["adapters"])
        self.assertIn("models", body)


if __name__ == "__main__":
    unittest.main()
