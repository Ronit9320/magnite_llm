"""Partial downloads: detecting unfinished Hub blobs and removing them safely.

Uses real Hub cache layouts in a temp directory; the shared cache is never touched.
"""
import os
from pathlib import Path
import tempfile
import unittest

from fastapi.testclient import TestClient

from backend.models.registry import ModelRegistry
from backend.models.schemas import ModelRecord, PullJob, model_id
from backend.models.store import ModelStore
from backend.routes import models
from backend.server import app


class PartialDownloads(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name).resolve()
        self.cache = root / "hub"
        self.cache.mkdir()
        self.store = ModelStore(str(root / "models.db"))
        self.registry = ModelRegistry(self.store, str(self.cache))

    def repo(self, repo="org/model", partial=2, complete=False):
        folder = self.cache / ("models--" + repo.replace("/", "--"))
        (folder / "blobs").mkdir(parents=True)
        for i in range(partial):
            (folder / "blobs" / f"blob{i}.incomplete").write_bytes(b"x" * 100)
        if complete:
            blob = folder / "blobs" / "done"
            blob.write_text('{"architectures":["T"]}')
            snap = folder / "snapshots" / "rev1"
            snap.mkdir(parents=True)
            (snap / "config.json").symlink_to(os.path.relpath(blob, snap))
        locks = self.cache / ".locks" / folder.name
        locks.mkdir(parents=True, exist_ok=True)
        (locks / "a.lock").write_text("")
        return folder

    def test_lists_only_repositories_with_unfinished_blobs(self):
        self.repo("org/partial")
        self.repo("org/finished", partial=0, complete=True)
        found = self.registry.list_partial_downloads()
        self.assertEqual([p["repo"] for p in found], ["org/partial"])
        self.assertEqual(found[0]["bytes"], 200)
        self.assertEqual(found[0]["files"], 2)
        self.assertFalse(found[0]["has_complete_files"])

    def test_active_download_is_not_reported_or_deleted(self):
        self.repo("org/live")
        self.registry._jobs["j"] = PullJob(id="j", ref="org/live", source="huggingface",
                                           status="running", progress=1.0, message="")
        self.assertEqual(self.registry.list_partial_downloads(), [])
        with self.assertRaises(RuntimeError):
            self.registry.delete_partial_download("org/live")

    def test_orphan_repository_is_removed_entirely(self):
        folder = self.repo("org/orphan")
        result = self.registry.delete_partial_download("org/orphan")
        self.assertTrue(result["folder_removed"])
        self.assertEqual(result["files_removed"], 2)
        self.assertFalse(folder.exists())
        self.assertFalse((self.cache / ".locks" / folder.name).exists())

    def test_usable_files_are_kept(self):
        folder = self.repo("org/kept", complete=True)
        result = self.registry.delete_partial_download("org/kept")
        self.assertFalse(result["folder_removed"])
        self.assertTrue((folder / "snapshots" / "rev1" / "config.json").is_file())
        self.assertEqual(list((folder / "blobs").glob("*.incomplete")), [])

    def test_registered_model_folder_is_kept(self):
        folder = self.repo("org/registered")
        self.store.upsert(ModelRecord(id=model_id("huggingface", "org/registered"), name="registered",
                                      source="huggingface", ref="org/registered", path=str(folder),
                                      status="ready"))
        self.assertFalse(self.registry.delete_partial_download("org/registered")["folder_removed"])
        self.assertTrue(folder.exists())

    def test_rejects_paths_outside_the_cache(self):
        for bad in ("../etc", "org/../x", "noslash", "a\\b/c"):
            with self.assertRaises(ValueError):
                self.registry.delete_partial_download(bad)

    def test_routes(self):
        self.repo("org/route")
        previous = models._registry
        models._registry = self.registry
        self.addCleanup(setattr, models, "_registry", previous)
        client = TestClient(app)
        listed = client.get("/models/partials")
        self.assertEqual(listed.status_code, 200)
        self.assertEqual(listed.json()[0]["repo"], "org/route")
        deleted = client.delete("/models/partials", params={"repo": "org/route"})
        self.assertEqual(deleted.status_code, 200)
        self.assertEqual(client.delete("/models/partials", params={"repo": "org/route"}).status_code, 404)


if __name__ == "__main__":
    unittest.main()
