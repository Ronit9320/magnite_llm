"""Deleting models: complete cleanup, the guards that protect work in progress,
and what the API tells the caller when something stops a delete.

Uses real Hub cache layouts in a temp directory; the shared cache is never touched.
"""
import os
from pathlib import Path
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import httpx

from backend.inference.supervisor import get_supervisor
from backend.models.registry import ModelRegistry
from backend.models.schemas import ModelRecord, model_id
from backend.models.store import ModelStore
from backend.routes import models, training
from backend.server import app


def tree(path):
    path = Path(path)
    return sorted(str(p.relative_to(path)) for p in path.rglob("*")) if path.exists() else []


class DeleteFixture(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name).resolve()
        self.cache = self.root / "hub"
        self.cache.mkdir()
        self.store = ModelStore(str(self.root / "models.db"))
        self.registry = ModelRegistry(self.store, str(self.cache))

    def cached_model(self, repo="org/model", revisions=("rev1",), main=None):
        """A real Hub cache layout, plus the lock files a download leaves behind."""
        folder = self.cache / ("models--" + repo.replace("/", "--"))
        (folder / "blobs").mkdir(parents=True, exist_ok=True)
        (folder / "refs").mkdir(exist_ok=True)
        for n, revision in enumerate(revisions):
            snap = folder / "snapshots" / revision
            snap.mkdir(parents=True)
            files = {"config.json": '{"architectures":["T"]}', "model.safetensors": f"weights-{revision}" * 4000}
            for i, (name, content) in enumerate(files.items()):
                blob = folder / "blobs" / f"{revision}-{i}"
                blob.write_text(content)
                (snap / name).symlink_to(os.path.relpath(blob, snap))
        (folder / "refs" / "main").write_text(main or revisions[-1])
        locks = self.cache / ".locks" / folder.name
        locks.mkdir(parents=True, exist_ok=True)
        (locks / "abc.lock").write_text("")
        return folder

    def synced(self):
        return self.registry.list_synced(source="huggingface")


class RegistryDeleteTests(DeleteFixture):
    def test_delete_removes_files_record_and_lock_folder(self):
        folder = self.cached_model()
        record = self.synced()[0]
        self.assertTrue(self.registry.delete_model(record.id))
        self.assertFalse(folder.exists())
        self.assertFalse((self.cache / ".locks" / folder.name).exists(), "the repo's lock files are cleaned up too")
        self.assertIsNone(self.registry.get_model(record.id))
        self.assertEqual(self.synced(), [])

    def test_delete_leaves_other_models_and_their_locks_alone(self):
        self.cached_model("org/keep")
        doomed = self.cached_model("org/doomed")
        keep_before = tree(self.cache / "models--org--keep")
        keep_locks = tree(self.cache / ".locks" / "models--org--keep")
        record = next(r for r in self.synced() if r.ref == "org/doomed")
        self.registry.delete_model(record.id)
        self.assertFalse(doomed.exists())
        self.assertEqual(tree(self.cache / "models--org--keep"), keep_before)
        self.assertEqual(tree(self.cache / ".locks" / "models--org--keep"), keep_locks)
        self.assertEqual([r.ref for r in self.synced()], ["org/keep"])

    def test_every_cached_revision_is_removed_so_the_model_cannot_reappear(self):
        folder = self.cached_model(revisions=("old1", "new2"), main="new2")
        record = self.synced()[0]
        self.assertTrue(self.registry.delete_model(record.id))
        self.assertFalse(folder.exists())
        self.assertEqual(self.synced(), [], "an older revision must not resurrect a deleted model")

    def test_repos_sharing_a_commit_hash_are_not_confused(self):
        # Forks and mirrors share commit hashes. The Hub cache API matches by hash across
        # repos in arbitrary order, so force the dangerous order and check both ways round.
        import dataclasses
        from huggingface_hub import scan_cache_dir as real_scan
        for doomed_first in (True, False):
            with self.subTest(doomed_first=doomed_first):
                self.setUp()
                self.cached_model("org/original", revisions=("samehash",))
                self.cached_model("fork/original", revisions=("samehash",))
                target = next(r for r in self.synced() if r.ref == "fork/original")
                keep = self.cache / "models--org--original"
                keep_before = tree(keep)

                def scan(path, *a, **k):
                    info = real_scan(path, *a, **k)
                    ordered = sorted(info.repos, key=lambda r: (r.repo_id != "fork/original") == doomed_first)
                    return dataclasses.replace(info, repos=tuple(ordered))
                with patch("huggingface_hub.scan_cache_dir", scan):
                    self.assertTrue(self.registry.delete_model(target.id))
                self.assertFalse((self.cache / "models--fork--original").exists())
                self.assertEqual(tree(keep), keep_before, "the other repo's files must be untouched")
                self.assertEqual([r.ref for r in self.synced()], ["org/original"])
                self.temp.cleanup()

    def test_partial_delete_preserves_blob_used_by_another_repo(self):
        doomed = self.cached_model("org/doomed", revisions=("old", "current"))
        keep = self.cached_model("org/keep")
        shared = self.cache / "shared-weight"
        shared.write_text("shared")
        for snapshot in (doomed / "snapshots/old", keep / "snapshots/rev1"):
            link = snapshot / "model.safetensors"
            link.unlink()
            link.symlink_to(os.path.relpath(shared, snapshot))
        record = ModelRecord(id="huggingface:org/doomed", name="doomed", source="huggingface",
                             ref="org/doomed", path=str(doomed / "snapshots/old"))
        self.registry._remove_hf(record)
        self.assertEqual((keep / "snapshots/rev1/model.safetensors").read_text(), "shared")
        self.assertFalse((doomed / "snapshots/old").exists())
        self.assertTrue((doomed / "snapshots/current").exists())

    def test_unknown_model_is_not_deleted(self):
        self.assertFalse(self.registry.delete_model("huggingface:nope"))

    def test_record_is_removed_even_if_the_files_are_already_gone(self):
        folder = self.cached_model()
        record = self.synced()[0]
        import shutil
        shutil.rmtree(folder)
        self.assertTrue(self.registry.delete_model(record.id))
        self.assertIsNone(self.registry.get_model(record.id))

    def test_unmanaged_path_is_refused_and_nothing_is_removed(self):
        stray = self.root / "my-own-model"
        stray.mkdir()
        (stray / "weights.bin").write_text("mine")
        self.store.upsert(ModelRecord(id="huggingface:stray", name="stray", source="huggingface",
                                      ref="stray", path=str(stray)))
        with self.assertRaises(ValueError):
            self.registry.delete_model("huggingface:stray")
        self.assertTrue((stray / "weights.bin").exists())
        self.assertIsNotNone(self.registry.get_model("huggingface:stray"))

    def test_delete_is_refused_while_that_model_is_downloading(self):
        self.cached_model()
        record = self.synced()[0]
        job_id = self.registry._create_job("org/model", "huggingface")
        self.registry._jobs[job_id].status = "running"
        with self.assertRaisesRegex(RuntimeError, "download"):
            self.registry.delete_model(record.id)
        self.assertIsNotNone(self.registry.get_model(record.id))
        self.assertTrue((self.cache / "models--org--model").exists())

    def test_a_download_of_a_different_model_does_not_block_delete(self):
        self.cached_model()
        record = self.synced()[0]
        job_id = self.registry._create_job("org/other", "huggingface")
        self.registry._jobs[job_id].status = "running"
        self.assertTrue(self.registry.delete_model(record.id))

    def test_ollama_server_error_keeps_the_record(self):
        self.store.upsert(ModelRecord(id="ollama:toy", name="toy", source="ollama", ref="toy", path="toy"))
        bad = httpx.Response(500, request=httpx.Request("DELETE", "http://ollama"))
        with patch("backend.models.registry.httpx.request", return_value=bad):
            with self.assertRaises(httpx.HTTPError):
                self.registry.delete_model("ollama:toy")
        self.assertIsNotNone(self.registry.get_model("ollama:toy"))

    def test_ollama_model_already_gone_upstream_is_removed_from_the_library(self):
        self.store.upsert(ModelRecord(id="ollama:toy", name="toy", source="ollama", ref="toy", path="toy"))
        gone = httpx.Response(404, request=httpx.Request("DELETE", "http://ollama"))
        with patch("backend.models.registry.httpx.request", return_value=gone):
            self.assertTrue(self.registry.delete_model("ollama:toy"))
        self.assertIsNone(self.registry.get_model("ollama:toy"))


class DeleteRouteTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.fixture = DeleteFixture()
        self.fixture.setUp()
        self.addCleanup(self.fixture.doCleanups)
        patcher = patch.object(models, "_registry", self.fixture.registry)
        patcher.start()
        self.addCleanup(patcher.stop)
        self.sup = get_supervisor()
        self._saved = (self.sup._engine, self.sup.model_ref)
        self.addCleanup(lambda: setattr(self.sup, "_engine", self._saved[0]))
        self.addCleanup(lambda: setattr(self.sup, "model_ref", self._saved[1]))
        self.client = httpx.AsyncClient(transport=httpx.ASGITransport(app=app), base_url="http://test")
        self.addAsyncCleanup(self.client.aclose)

    def prepare(self, repo="org/model", **kw):
        folder = self.fixture.cached_model(repo, **kw)
        record = next(r for r in self.fixture.synced() if r.ref == repo)
        return folder, record

    async def delete(self, identifier):
        return await self.client.delete(f"/models/{identifier}")

    async def test_delete_succeeds_with_204_and_removes_everything(self):
        folder, record = self.prepare()
        r = await self.delete(record.id)
        self.assertEqual(r.status_code, 204)
        self.assertFalse(folder.exists())
        listed = (await self.client.get("/models", params={"source": "huggingface"})).json()
        self.assertEqual(listed, [])

    async def test_second_delete_is_404(self):
        _, record = self.prepare()
        self.assertEqual((await self.delete(record.id)).status_code, 204)
        r = await self.delete(record.id)
        self.assertEqual(r.status_code, 404)
        self.assertIn("not found", r.json()["detail"].lower())

    async def test_unknown_model_is_404(self):
        self.assertEqual((await self.delete("huggingface:nope--nope")).status_code, 404)

    async def test_loaded_model_is_409_with_an_actionable_reason(self):
        folder, record = self.prepare()
        self.sup._engine, self.sup.model_ref = object(), record.id
        r = await self.delete(record.id)
        self.assertEqual(r.status_code, 409)
        self.assertIn("Unload", r.json()["detail"])
        self.assertTrue(folder.exists())
        self.assertIsNotNone(self.fixture.registry.get_model(record.id))

    async def test_model_loaded_by_repo_name_is_also_protected(self):
        folder, record = self.prepare()
        self.sup._engine, self.sup.model_ref = object(), record.ref
        self.assertEqual((await self.delete(record.id)).status_code, 409)
        self.assertTrue(folder.exists())

    async def test_a_different_loaded_model_does_not_block_delete(self):
        _, record = self.prepare()
        self.sup._engine, self.sup.model_ref = object(), "huggingface:someone--else"
        self.assertEqual((await self.delete(record.id)).status_code, 204)

    async def test_model_used_by_active_training_is_409(self):
        folder, record = self.prepare()
        manager = SimpleNamespace(list=lambda: [SimpleNamespace(status="running", base_model=record.ref)])
        with patch.object(training, "_manager", manager):
            r = await self.delete(record.id)
        self.assertEqual(r.status_code, 409)
        self.assertIn("training", r.json()["detail"].lower())
        self.assertTrue(folder.exists())

    async def test_finished_training_does_not_block_delete(self):
        _, record = self.prepare()
        manager = SimpleNamespace(list=lambda: [SimpleNamespace(status="done", base_model=record.ref)])
        with patch.object(training, "_manager", manager):
            self.assertEqual((await self.delete(record.id)).status_code, 204)

    async def test_model_being_downloaded_is_409(self):
        folder, record = self.prepare()
        job_id = self.fixture.registry._create_job(record.ref, "huggingface")
        self.fixture.registry._jobs[job_id].status = "running"
        r = await self.delete(record.id)
        self.assertEqual(r.status_code, 409)
        self.assertIn("download", r.json()["detail"].lower())
        self.assertTrue(folder.exists())

    async def test_unmanaged_path_is_400_and_files_survive(self):
        stray = self.fixture.root / "stray"
        stray.mkdir()
        (stray / "w.bin").write_text("mine")
        self.fixture.store.upsert(ModelRecord(id="huggingface:stray", name="stray", source="huggingface",
                                              ref="stray", path=str(stray)))
        r = await self.delete("huggingface:stray")
        self.assertEqual(r.status_code, 400)
        self.assertTrue((stray / "w.bin").exists())

    async def test_ollama_unreachable_is_502_and_keeps_the_record(self):
        self.fixture.store.upsert(ModelRecord(id="ollama:toy", name="toy", source="ollama", ref="toy", path="toy"))
        with patch("backend.models.registry.httpx.request", side_effect=httpx.ConnectError("offline")):
            r = await self.delete("ollama:toy")
        self.assertEqual(r.status_code, 502)
        self.assertIn("remains", r.json()["detail"])
        self.assertIsNotNone(self.fixture.registry.get_model("ollama:toy"))

    async def test_deleting_one_model_does_not_affect_another(self):
        keep_folder, keep = self.prepare("org/keep")
        _, doomed = self.prepare("org/doomed")
        self.assertEqual((await self.delete(doomed.id)).status_code, 204)
        self.assertTrue(keep_folder.exists())
        listed = (await self.client.get("/models", params={"source": "huggingface"})).json()
        self.assertEqual([m["ref"] for m in listed], ["org/keep"])

    async def test_deleted_model_is_no_longer_marked_downloaded_in_search(self):
        _, record = self.prepare()
        self.assertIn("org/model", self.fixture.registry._downloaded_hub_repos())
        await self.delete(record.id)
        self.assertNotIn("org/model", self.fixture.registry._downloaded_hub_repos())


if __name__ == "__main__":
    unittest.main()
