import glob
import os
import shutil
import tempfile
import threading
import unittest
import zlib
from unittest.mock import patch

import httpx
import numpy as np

from backend.rag import embeddings, memguard
from backend.rag.batching import IngestCancelled, embed_batches
from backend.rag.memguard import MemoryGuardTripped
from backend.rag.store import RagStore
from backend.routes import rag
from backend.server import app

OLD, NEW = "old-model", "new-model"
_real_remove = os.remove


def remove_like_windows(path):
    """Windows refuses to delete a file this process still has memory-mapped; Linux allows it.
    Checking /proc/self/maps reproduces the Windows behavior on Linux."""
    try:
        with open("/proc/self/maps", encoding="utf-8", errors="replace") as maps:
            if any(line.rstrip().endswith(os.path.abspath(path)) for line in maps):
                raise PermissionError(f"[like Windows] {path} is still mapped")
    except FileNotFoundError:
        pass
    return _real_remove(path)


def make_embed(dim):
    """Deterministic unit vectors from text, so a chunk's own text always finds that chunk."""
    def embed(texts):
        out = np.stack([np.random.default_rng(zlib.crc32(t.encode())).standard_normal(dim) for t in texts])
        out = out.astype(np.float32)
        return out / np.linalg.norm(out, axis=1, keepdims=True)
    return embed


class EmbedBatchesTests(unittest.TestCase):
    def run_batches(self, batches, **kw):
        writes = []
        n = embed_batches(batches, lambda b: np.ones((len(b), 2), np.float32),
                          lambda off, v: writes.append((off, v.shape[0])), **kw)
        return n, writes

    def test_writes_at_running_offsets_and_reports_progress(self):
        progress = []
        n, writes = self.run_batches([["a", "b", "c"], ["d"], ["e", "f"]], progress=progress.append)
        self.assertEqual((n, writes), (6, [(0, 3), (3, 1), (4, 2)]))
        self.assertEqual(progress, [3, 4, 6])

    def test_empty_input_does_nothing(self):
        self.assertEqual(self.run_batches([]), (0, []))

    def test_cancel_is_checked_before_every_batch(self):
        cancelled = threading.Event()
        writes = []

        def write(offset, vecs):
            writes.append(offset)
            if len(writes) == 2:
                cancelled.set()

        with self.assertRaises(IngestCancelled):
            embed_batches([["a"], ["b"], ["c"], ["d"]], lambda b: np.ones((1, 2), np.float32), write,
                          cancelled=cancelled)
        self.assertEqual(writes, [0, 1])                       # batch 3 never started

    def test_already_cancelled_embeds_nothing(self):
        cancelled = threading.Event()
        cancelled.set()
        calls = []
        with self.assertRaises(IngestCancelled):
            embed_batches([["a"]], lambda b: calls.append(b) or np.ones((1, 2), np.float32),
                          lambda o, v: None, cancelled=cancelled)
        self.assertEqual(calls, [])

    def test_embed_batches_stops_when_guard_trips(self):
        class Guard:
            n = 0

            def check(self):
                Guard.n += 1
                if Guard.n == 3:
                    raise MemoryGuardTripped("Embedding stopped: test")

        writes = []
        with self.assertRaises(MemoryGuardTripped):
            embed_batches([["a"], ["b"], ["c"], ["d"]], lambda b: np.ones((1, 2), np.float32),
                          lambda o, v: writes.append(o), guard=Guard())
        self.assertEqual(writes, [0, 1])

    def test_wrong_row_count_is_an_error(self):
        with self.assertRaises(ValueError):
            embed_batches([["a", "b"]], lambda b: np.ones((1, 2), np.float32), lambda o, v: None)


class StoreTestCase(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        windows_like = patch("backend.rag.store.os.remove", remove_like_windows)
        windows_like.start()
        self.addCleanup(windows_like.stop)
        self.store = RagStore(self.dir)
        self.store.update_config({"model": OLD})
        self.texts = [[f"doc{d} chunk {i} unique words {d * 100 + i}" for i in range(5)] for d in range(3)]
        self.embed_old = make_embed(8)
        self.docs = [self.store.add_document(f"doc{d}", "txt", "", OLD, chunks, self.embed_old(chunks), 1.0, 64, 8)
                     for d, chunks in enumerate(self.texts)]

    def snapshot(self):
        with self.store._conn() as conn:
            chunks = [(r["id"], bytes(r["embedding"])) for r in conn.execute("SELECT id, embedding FROM rag_chunks ORDER BY id")]
            models = [r["model"] for r in conn.execute("SELECT model FROM rag_documents ORDER BY id")]
        cfg = self.store.get_config()
        return chunks, models, (cfg.model, cfg.truncate_dim)

    def leftovers(self):
        return glob.glob(os.path.join(self.dir, "reindex-*"))

    def old_search_works(self):
        hit = self.store.search(self.embed_old([self.texts[1][2]])[0], OLD, top_k=1)[0]
        self.assertEqual(hit.text, self.texts[1][2])


class ReindexTests(StoreTestCase):
    def test_reindex_changes_model_dim_and_search(self):
        embed_new = make_embed(16)
        cfg = self.store.reindex(NEW, 0, embed_new, batch=4)
        self.assertEqual(cfg.model, NEW)
        chunks, models, config = self.snapshot()
        self.assertEqual(models, [NEW] * 3)
        self.assertEqual(config, (NEW, 0))
        self.assertTrue(all(len(blob) == 16 * 4 for _, blob in chunks))
        hit = self.store.search(embed_new([self.texts[2][3]])[0], NEW, top_k=1)[0]
        self.assertEqual(hit.text, self.texts[2][3])
        self.assertEqual(self.store.search(self.embed_old([self.texts[2][3]])[0], OLD), [])
        self.assertEqual(self.leftovers(), [])

    def test_reindex_stores_truncate_dim_and_keeps_other_config(self):
        self.store.update_config({"top_k": 9})
        self.store.reindex(NEW, 512, make_embed(16))
        cfg = self.store.get_config()
        self.assertEqual((cfg.model, cfg.truncate_dim, cfg.top_k), (NEW, 512, 9))

    def test_embeds_in_bounded_batches_in_chunk_order(self):
        seen = []

        def embed(texts):
            seen.append(list(texts))
            return make_embed(16)(texts)

        self.store.reindex(NEW, 0, embed, batch=4)
        self.assertEqual([len(b) for b in seen], [4, 4, 4, 3])
        self.assertEqual(sum(seen, []), sum(self.texts, []))

    def test_progress_is_reported(self):
        progress = []
        self.store.reindex(NEW, 0, make_embed(16), batch=4, progress=progress.append)
        self.assertEqual(progress, [4, 8, 12, 15])

    def test_same_model_and_size_is_a_noop(self):
        before = self.snapshot()
        cfg = self.store.reindex(OLD, 0, lambda t: self.fail("must not embed"))
        self.assertEqual(cfg.model, OLD)
        self.assertEqual(self.snapshot(), before)

    def test_empty_store_just_switches_the_config(self):
        self.store.clear()
        cfg = self.store.reindex(NEW, 256, lambda t: self.fail("nothing to embed"))
        self.assertEqual((cfg.model, cfg.truncate_dim), (NEW, 256))

    def test_old_index_keeps_serving_while_a_reindex_runs(self):
        during = []

        def embed(texts):
            during.append(self.store.search(self.embed_old([self.texts[0][0]])[0], OLD, top_k=1)[0].text)
            return make_embed(16)(texts)

        self.store.reindex(NEW, 0, embed, batch=8)
        self.assertEqual(during, [self.texts[0][0]] * 2)


class ReindexSafetyTests(StoreTestCase):
    def assert_untouched(self, before):
        self.assertEqual(self.snapshot(), before)
        self.old_search_works()
        self.assertFalse(self.store.reindexing)
        self.assertEqual(self.leftovers(), [])

    def test_failure_midway_leaves_everything_untouched(self):
        before = self.snapshot()
        calls = {"n": 0}

        def embed(texts):
            calls["n"] += 1
            if calls["n"] == 3:
                raise RuntimeError("embedder crashed")
            return make_embed(16)(texts)

        with self.assertRaises(RuntimeError):
            self.store.reindex(NEW, 0, embed, batch=4)
        self.assert_untouched(before)

    def test_cancel_leaves_everything_untouched(self):
        before = self.snapshot()
        cancelled = threading.Event()

        def embed(texts):
            cancelled.set()
            return make_embed(16)(texts)

        with self.assertRaises(IngestCancelled):
            self.store.reindex(NEW, 0, embed, cancelled=cancelled, batch=4)
        self.assert_untouched(before)

    def test_failure_during_apply_rolls_back(self):
        before = self.snapshot()
        with self.store._conn() as conn:
            conn.execute("CREATE TRIGGER boom BEFORE UPDATE ON rag_chunks WHEN NEW.id = 9 "
                         "BEGIN SELECT RAISE(ABORT, 'boom'); END")
            conn.commit()
        with self.assertRaises(Exception):
            self.store.reindex(NEW, 0, make_embed(16), batch=4)
        with self.store._conn() as conn:
            conn.execute("DROP TRIGGER boom")
            conn.commit()
        self.assert_untouched(before)

    def test_inconsistent_dimensions_between_batches_are_rejected(self):
        before = self.snapshot()
        calls = {"n": 0}

        def embed(texts):
            calls["n"] += 1
            return make_embed(16 if calls["n"] == 1 else 12)(texts)

        with self.assertRaises(ValueError):
            self.store.reindex(NEW, 0, embed, batch=4)
        self.assert_untouched(before)

    def test_writes_are_blocked_while_reindexing(self):
        errors = []

        def embed(texts):
            self.assertTrue(self.store.reindexing)
            for name, call in (
                    ("add", lambda: self.store.add_document("x", "txt", "", NEW, ["x"], make_embed(16)(["x"]), 1.0, 64, 8)),
                    ("delete", lambda: self.store.delete_document(self.docs[0].id)),
                    ("clear", self.store.clear),
                    ("reindex", lambda: self.store.reindex(NEW, 0, make_embed(16)))):
                with self.assertRaises(ValueError) as ctx:
                    call()
                errors.append((name, str(ctx.exception)))
            return make_embed(16)(texts)

        self.store.reindex(NEW, 0, embed, batch=100)
        self.assertEqual([n for n, _ in errors], ["add", "delete", "clear", "reindex"])
        self.assertTrue(all("Reindex in progress" in m for _, m in errors))
        self.assertFalse(self.store.reindexing)
        self.assertEqual(self.store.count_chunks(), 15)

    def test_temp_file_exists_only_during_the_run_and_stale_ones_are_cleaned_on_startup(self):
        seen = []

        def embed(texts):
            seen.append(len(self.leftovers()))
            return make_embed(16)(texts)

        self.store.reindex(NEW, 0, embed, batch=100)
        self.assertEqual(seen, [0])                                    # the temp file is created on the first write
        self.assertEqual(self.leftovers(), [])
        stale = os.path.join(self.dir, "reindex-deadbeef.tmp")
        with open(stale, "wb") as handle:
            handle.write(b"junk")
        RagStore(self.dir)
        self.assertFalse(os.path.exists(stale))

    def test_guard_trip_leaves_everything_untouched(self):
        before = self.snapshot()

        class Guard:
            n = 0

            def check(self):
                Guard.n += 1
                if Guard.n == 2:
                    raise MemoryGuardTripped("Embedding stopped: test")

        with self.assertRaises(MemoryGuardTripped):
            self.store.reindex(NEW, 0, make_embed(16), batch=4, guard=Guard())
        self.assert_untouched(before)


class RouteTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        env = patch.dict(os.environ, {"MAGNITE_HOME": self.temp.name})
        env.start()
        self.addCleanup(env.stop)
        rag._rag_setup()
        self.store = rag._get_store()
        self.store.update_config({"model": OLD})
        dims = {OLD: 8, NEW: 16, embeddings.MINILM: 384}

        def embed_documents(texts, model, truncate_dim=0):
            return make_embed(dims.get(model, 8))(texts)

        def embed_query(text, model, truncate_dim=0):
            return make_embed(dims.get(model, 8))([text])[0]

        for p in (patch.object(embeddings, "is_available", return_value=True),
                  patch.object(embeddings, "embed_documents", side_effect=embed_documents),
                  patch.object(embeddings, "embed_query", side_effect=embed_query),
                  patch.object(memguard, "available_memory_mb", return_value=50_000.0)):
            p.start()
            self.addCleanup(p.stop)
        self.client = httpx.AsyncClient(transport=httpx.ASGITransport(app=app, raise_app_exceptions=False),
                                        base_url="http://test")
        self.addAsyncCleanup(self.client.aclose)
        r = await self.client.post("/rag/documents", json={"name": "facts.txt", "content": "Paris has the Eiffel Tower."})
        self.assertEqual(r.status_code, 201, r.text)

    async def test_reindex_happy_path(self):
        r = await self.client.post("/rag/reindex", json={"model": NEW})
        self.assertEqual(r.status_code, 200, r.text)
        self.assertEqual(r.json()["model"], NEW)
        q = await self.client.post("/rag/query", json={"query": "Paris has the Eiffel Tower."})
        self.assertEqual(q.json()["results"][0]["text"], "Paris has the Eiffel Tower.")
        status = (await self.client.get("/rag/status")).json()
        self.assertEqual(status["model"], NEW)

    async def test_empty_model_is_400(self):
        for body in ({"model": ""}, {"model": "   "}):
            self.assertEqual((await self.client.post("/rag/reindex", json=body)).status_code, 400)

    async def test_same_model_is_a_noop_200(self):
        r = await self.client.post("/rag/reindex", json={"model": OLD})
        self.assertEqual(r.status_code, 200, r.text)
        self.assertEqual(r.json()["model"], OLD)

    async def test_unsupported_truncation_is_400_and_changes_nothing(self):
        r = await self.client.post("/rag/reindex", json={"model": embeddings.MINILM, "truncate_dim": 128})
        self.assertEqual(r.status_code, 400, r.text)
        self.assertEqual(self.store.get_config().model, OLD)

    async def test_unavailable_embeddings_is_503(self):
        with patch.object(embeddings, "is_available", return_value=False):
            r = await self.client.post("/rag/reindex", json={"model": NEW})
        self.assertEqual(r.status_code, 503)

    async def test_ingest_during_reindex_returns_409(self):
        self.store._reindexing = True
        try:
            r = await self.client.post("/rag/documents", json={"name": "b.txt", "content": "more text"})
        finally:
            self.store._reindexing = False
        self.assertEqual(r.status_code, 409, r.text)
        self.assertIn("Reindex in progress", r.text)

    async def test_delete_during_reindex_returns_409(self):
        doc = self.store.list_documents()[0].id
        self.store._reindexing = True
        try:
            r = await self.client.delete(f"/rag/documents/{doc}")
        finally:
            self.store._reindexing = False
        self.assertEqual(r.status_code, 409, r.text)

    async def test_embedding_failure_is_503_and_leaves_the_index_alone(self):
        with patch.object(embeddings, "embed_documents", side_effect=RuntimeError("model exploded")):
            r = await self.client.post("/rag/reindex", json={"model": NEW})
        self.assertEqual(r.status_code, 503, r.text)
        self.assertEqual(self.store.get_config().model, OLD)
        self.assertFalse(self.store.reindexing)

    async def test_memory_guard_trip_is_503_with_the_message(self):
        with patch.object(embeddings, "embed_documents", side_effect=MemoryGuardTripped("Not enough free memory: test")):
            r = await self.client.post("/rag/reindex", json={"model": NEW})
        self.assertEqual(r.status_code, 503, r.text)
        self.assertIn("free memory", r.text)

    async def test_config_model_change_is_still_blocked_with_documents(self):
        r = await self.client.post("/rag/config", json={"model": NEW})
        self.assertEqual(r.status_code, 409, r.text)


if __name__ == "__main__":
    unittest.main()
