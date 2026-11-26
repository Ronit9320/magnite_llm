"""The build-time feature profile decides which HTTP surfaces exist."""
import os
from pathlib import Path
import re
import tempfile
import unittest
from unittest.mock import patch

from fastapi.testclient import TestClient

from backend import _features
from backend.server import app

ROOT = Path(__file__).resolve().parents[2]


class FeatureProfileTests(unittest.TestCase):
    def test_profile_values_are_plain_literals_for_cmake(self):
        text = (ROOT / "backend/src/backend/_features.py").read_text()
        self.assertRegex(text, r"(?m)^BLUEPRINTS_AND_TOOLS = (True|False)$")
        self.assertIsInstance(_features.BLUEPRINTS_AND_TOOLS, bool)

    def test_routes_follow_the_profile(self):
        paths = set(app.openapi()["paths"])
        enabled = _features.BLUEPRINTS_AND_TOOLS
        self.assertEqual("/blueprints" in paths, enabled)
        self.assertEqual("/inference/tool-runs" in paths, enabled)
        self.assertEqual(any(re.match(r"^/blueprints/runs/.+/approvals", p) for p in paths), enabled)

    @unittest.skipIf(_features.BLUEPRINTS_AND_TOOLS, "only meaningful when the features are switched off")
    def test_switched_off_build_creates_no_blueprint_storage(self):
        with tempfile.TemporaryDirectory() as home, \
                patch.dict(os.environ, {"MAGNITE_HOME": home}), \
                patch("backend.runtime_packages.activate"):
            with TestClient(app) as client:
                self.assertEqual(client.get("/blueprints").status_code, 404)
                self.assertEqual(client.post("/inference/tool-runs", json={}).status_code, 404)
            self.assertFalse((Path(home) / "blueprints.db").exists())


if __name__ == "__main__":
    unittest.main()
