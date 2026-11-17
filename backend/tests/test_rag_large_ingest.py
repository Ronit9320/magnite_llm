import asyncio
import os
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

import httpx
import numpy as np
from fastapi import HTTPException

from backend.rag import activity as activity_module
from backend.rag import embeddings, memguard
from backend.rag.activity import exclusive, get_activity
from backend.rag.batching import IngestCancelled
from backend.rag.memguard import MemoryGuardTripped
from backend.rag.schemas import RagIngestRequest
from backend.routes import rag
from backend.server import app

MODEL = "fake-model"


def vectors(n, dim=4):
    return np.ones((n, dim), np.float32) / np.sqrt(dim)


def words(n):
    return " ".join(f"w{i}" for i in range(n))


class ActivityTests(unittest.TestCase):
    def setUp(self):
        self.act = get_activity()
        self.addCleanup(self.act.finish)

    def test_idle_snapshot(self):
        self.act.finish()
        self.assertEqual(self.act.snapshot(), {"active": False, "kind": None, "name": "", "phase": "",
                                               "done": 0, "total": 0, "waiting": 0})

    def test_lifecycle(self):
        self.act.start("ingest", "book.pdf")
        self.act.set_phase("embedding", 500)
        self.act.advance(120)
        snap = self.act.snapshot()
        self.assertEqual((snap["active"], snap["kind"], snap["name"], snap["phase"], snap["done"], snap["total"]),
                         (True, "ingest", "book.pdf", "embedding", 120, 500))
        self.act.finish()
        self.assertFalse(self.act.snapshot()["active"])

    def test_set_phase_without_total_keeps_total_and_resets_done_on_new_total(self):
        self.act.start("ingest", "x")
        self.act.set_phase("embedding", 10)
        self.act.advance(4)
        self.act.set_phase("saving")
        self.assertEqual((self.act.snapshot()["total"], self.act.snapshot()["done"]), (10, 4))
        self.act.set_phase("embedding", 20)
        self.assertEqual(self.act.snapshot()["done"], 0)

    def test_exclusive_serializes_and_reports_waiting(self):
        order, release = [], threading.Event()
        first_in = threading.Event()

        def first():
            with exclusive("ingest", "a"):
                first_in.set()
                order.append("a-in")
                release.wait(5)
                order.append("a-out")

        def second():
            with exclusive("ingest", "b"):
                order.append("b-in")

        t1 = threading.Thread(target=first)
        t1.start()
        self.assertTrue(first_in.wait(5))
        t2 = threading.Thread(target=second)
        t2.start()
        deadline = time.time() + 5
        while self.act.snapshot()["waiting"] != 1 and time.time() < deadline:
            time.sleep(0.01)
        self.assertEqual(self.act.snapshot()["waiting"], 1)
        self.assertEqual(self.act.snapshot()["name"], "a")
        release.set()
        t1.join(5)
        t2.join(5)
        self.assertEqual(order, ["a-in", "a-out", "b-in"])
        self.assertEqual(self.act.snapshot()["waiting"], 0)

    def test_cancelled_waiter_leaves_immediately_without_taking_the_lock(self):
        release, first_in = threading.Event(), threading.Event()

        def first():
            with exclusive("ingest", "a"):
                first_in.set()
                release.wait(5)

        t1 = threading.Thread(target=first)
        t1.start()
        self.assertTrue(first_in.wait(5))
        cancelled = threading.Event()
        result = {}

        def waiter():
            try:
                with exclusive("ingest", "b", cancelled):
                    result["entered"] = True
            except IngestCancelled:
                result["cancelled"] = time.time()

        t2 = threading.Thread(target=waiter)
        t2.start()
        time.sleep(0.2)
        started = time.time()
        cancelled.set()
        t2.join(3)
        self.assertFalse(t2.is_alive())
        self.assertLess(result["cancelled"] - started, 1.0)
        self.assertNotIn("entered", result)
        self.assertEqual(self.act.snapshot()["name"], "a")              # the running operation is unaffected
        release.set()
        t1.join(5)

    def test_already_cancelled_never_starts_even_when_idle(self):
        cancelled = threading.Event()
        cancelled.set()
        with self.assertRaises(IngestCancelled):
            with exclusive("ingest", "x", cancelled):
                self.fail("must not run")
        with exclusive("ingest", "y"):                                  # and the lock was not leaked
            pass

    def test_lock_is_released_when_the_body_raises(self):
        with self.assertRaises(RuntimeError):
            with exclusive("ingest", "x"):
                raise RuntimeError("boom")
        self.assertFalse(self.act.snapshot()["active"])
        with exclusive("ingest", "y"):
            pass


class IngestTestCase(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        env = patch.dict(os.environ, {"MAGNITE_HOME": self.temp.name})
        env.start()
        self.addCleanup(env.stop)
        rag._rag_setup()
        self.store = rag._get_store()
        self.store.update_config({"model": MODEL})
        self.calls = []                      # one entry per embed_documents call: (n texts, activity snapshot)
        self.embed_hook = None

        def embed_documents(texts, model, truncate_dim=0):
            self.calls.append((len(texts), get_activity().snapshot()))
            if self.embed_hook:
                self.embed_hook(len(self.calls))
            return vectors(len(texts))

        for p in (patch.object(embeddings, "is_available", return_value=True),
                  patch.object(embeddings, "embed_documents", side_effect=embed_documents),
                  patch.object(embeddings, "embed_query", side_effect=lambda t, m, d=0: vectors(1)[0]),
                  patch.object(memguard, "available_memory_mb", return_value=50_000.0)):
            p.start()
            self.addCleanup(p.stop)
        self.client = httpx.AsyncClient(transport=httpx.ASGITransport(app=app, raise_app_exceptions=False),
                                        base_url="http://test")
        self.addAsyncCleanup(self.client.aclose)
        self.addCleanup(get_activity().finish)

    def request(self, n_words, name="big.txt", chunk_size=64, overlap=0):
        return RagIngestRequest(name=name, content=words(n_words), chunk_size=chunk_size, chunk_overlap=overlap)


class BatchingTests(IngestTestCase):
    async def test_20k_chunks_are_embedded_in_batches_of_at_most_16(self):
        doc = await asyncio.to_thread(rag._ingest_document, self.request(64 * 20_000))
        self.assertEqual(doc.chunks, 20_000)
        self.assertEqual(self.store.count_chunks(), 20_000)
        sizes = [n for n, _ in self.calls]
        self.assertLessEqual(max(sizes), 16)
        self.assertEqual(sum(sizes), 20_000)
        self.assertEqual(len(sizes), 1250)

    async def test_batch_size_env_is_respected(self):
        with patch.dict(os.environ, {"MAGNITE_RAG_BATCH": "4"}):
            await asyncio.to_thread(rag._ingest_document, self.request(64 * 10))
        self.assertEqual([n for n, _ in self.calls], [4, 4, 2])

    async def test_bad_batch_size_env_falls_back_to_default(self):
        with patch.dict(os.environ, {"MAGNITE_RAG_BATCH": "nonsense"}):
            await asyncio.to_thread(rag._ingest_document, self.request(64 * 40))
        self.assertEqual([n for n, _ in self.calls], [16, 16, 8])

    async def test_vectors_land_on_the_right_chunks(self):
        def embed(texts, model, truncate_dim=0):
            out = np.zeros((len(texts), 4), np.float32)
            for i, t in enumerate(texts):                      # encode the first word's number in the vector
                out[i, 0] = 1.0
                out[i, 1] = float(t.split()[0][1:]) / 1e6
            return out

        with patch.object(embeddings, "embed_documents", side_effect=embed):
            await asyncio.to_thread(rag._ingest_document, self.request(64 * 40))
        with self.store._conn() as conn:
            rows = conn.execute("SELECT chunk_index, text, embedding FROM rag_chunks ORDER BY chunk_index").fetchall()
        for row in rows:
            vec = np.frombuffer(row["embedding"], np.float32)
            self.assertAlmostEqual(float(vec[1]) * 1e6, float(row["text"].split()[0][1:]), places=3)


class ProgressTests(IngestTestCase):
    async def test_progress_phases_and_final_state(self):
        await asyncio.to_thread(rag._ingest_document, self.request(64 * 40, name="book.txt"))
        first, last = self.calls[0][1], self.calls[-1][1]
        self.assertTrue(first["active"])
        self.assertEqual((first["kind"], first["name"], first["phase"], first["total"]), ("ingest", "book.txt", "embedding", 40))
        self.assertEqual([snap["done"] for _, snap in self.calls], [0, 16, 32])
        self.assertEqual(get_activity().snapshot()["active"], False)

    async def test_activity_route_reports_an_active_ingest_then_idle(self):
        started, release = threading.Event(), threading.Event()

        def block(call_no):
            started.set()
            release.wait(10)

        self.embed_hook = block
        worker = threading.Thread(target=lambda: rag._ingest_document(self.request(64 * 40, name="slow.txt")))
        worker.start()
        self.assertTrue(await asyncio.to_thread(started.wait, 5))
        data = (await self.client.get("/rag/activity")).json()
        self.assertTrue(data["active"])
        self.assertEqual((data["kind"], data["name"], data["phase"]), ("ingest", "slow.txt", "embedding"))
        release.set()
        await asyncio.to_thread(worker.join, 10)
        idle = (await self.client.get("/rag/activity")).json()
        self.assertFalse(idle["active"])

    async def test_reindex_reports_activity_too(self):
        await asyncio.to_thread(rag._ingest_document, self.request(64 * 20))
        self.calls.clear()
        with patch.object(embeddings, "spec_for", wraps=embeddings.spec_for):
            await asyncio.to_thread(rag._reindex, rag.RagReindexRequest(model="other-model"))
        snap = self.calls[0][1]
        self.assertEqual((snap["kind"], snap["phase"], snap["total"]), ("reindex", "embedding", 20))


class CancelAndFailureTests(IngestTestCase):
    async def test_cancel_after_second_batch_stops_and_stores_nothing(self):
        cancelled = threading.Event()
        self.embed_hook = lambda n: cancelled.set() if n == 2 else None
        with self.assertRaises(HTTPException) as ctx:
            await asyncio.to_thread(rag._ingest_document, self.request(64 * 80), cancelled)
        self.assertEqual(ctx.exception.status_code, 499)
        self.assertIn("cancelled", ctx.exception.detail)
        self.assertEqual(len(self.calls), 2)                      # batch 3 never started
        self.assertEqual(self.store.list_documents(), [])
        self.assertEqual(self.store.count_chunks(), 0)
        self.assertFalse(get_activity().snapshot()["active"])

    async def test_cancel_before_start_embeds_nothing(self):
        cancelled = threading.Event()
        cancelled.set()
        with self.assertRaises(HTTPException):
            await asyncio.to_thread(rag._ingest_document, self.request(64 * 5), cancelled)
        self.assertEqual(self.calls, [])

    async def test_failure_midway_stores_nothing_and_does_not_wedge_later_ingests(self):
        def boom(n):
            if n == 3:
                raise RuntimeError("encoder exploded")

        self.embed_hook = boom
        with self.assertRaises(HTTPException) as ctx:
            await asyncio.to_thread(rag._ingest_document, self.request(64 * 80))
        self.assertEqual(ctx.exception.status_code, 503)
        self.assertEqual(self.store.count_chunks(), 0)
        self.embed_hook = None
        doc = await asyncio.to_thread(rag._ingest_document, self.request(64 * 3, name="ok.txt"))
        self.assertEqual(doc.chunks, 3)

    async def test_memory_guard_trip_midway_is_503_and_stores_nothing(self):
        class Guard:
            n = 0

            def check(self):
                Guard.n += 1
                if Guard.n == 3:
                    raise MemoryGuardTripped("Embedding stopped: test limit")

        with patch.object(rag, "MemoryGuard", return_value=Guard()):
            with self.assertRaises(HTTPException) as ctx:
                await asyncio.to_thread(rag._ingest_document, self.request(64 * 80))
        self.assertEqual(ctx.exception.status_code, 503)
        self.assertIn("test limit", ctx.exception.detail)
        self.assertEqual(len(self.calls), 2)
        self.assertEqual(self.store.count_chunks(), 0)

    async def test_model_changed_during_ingest_is_still_409_and_stores_nothing(self):
        self.embed_hook = lambda n: self.store.update_config({"model": "changed"}) if n == 1 else None
        with self.assertRaises(HTTPException) as ctx:
            await asyncio.to_thread(rag._ingest_document, self.request(64 * 5))
        self.assertEqual(ctx.exception.status_code, 409)
        self.assertEqual(self.store.count_chunks(), 0)


class SerializationTests(IngestTestCase):
    async def test_second_ingest_waits_for_the_first_and_both_are_stored(self):
        started, release = threading.Event(), threading.Event()
        order = []

        def block(call_no):
            if call_no == 1:
                started.set()
                release.wait(10)
            order.append(call_no)

        self.embed_hook = block
        t1 = threading.Thread(target=lambda: rag._ingest_document(self.request(64 * 20, name="a.txt")))
        t1.start()
        self.assertTrue(await asyncio.to_thread(started.wait, 5))
        t2 = threading.Thread(target=lambda: rag._ingest_document(self.request(64 * 20, name="b.txt")))
        t2.start()
        await asyncio.sleep(0.3)
        self.assertEqual(len(self.calls), 1)                      # b has not embedded anything yet
        self.assertEqual(get_activity().snapshot()["waiting"], 1)
        release.set()
        await asyncio.to_thread(t1.join, 10)
        await asyncio.to_thread(t2.join, 10)
        self.assertEqual({d.name for d in self.store.list_documents()}, {"a.txt", "b.txt"})

    async def test_ingest_while_reindexing_is_refused_early_without_embedding(self):
        self.store._reindexing = True
        try:
            with self.assertRaises(HTTPException) as ctx:
                await asyncio.to_thread(rag._ingest_document, self.request(64 * 5))
        finally:
            self.store._reindexing = False
        self.assertEqual(ctx.exception.status_code, 409)
        self.assertEqual(self.calls, [])

    async def test_small_documents_still_work_through_the_http_route(self):
        r = await self.client.post("/rag/documents", json={"name": "tiny.txt", "content": "Paris is in France."})
        self.assertEqual(r.status_code, 201, r.text)
        self.assertEqual(r.json()["chunks"], 1)
        q = await self.client.post("/rag/query", json={"query": "Paris"})
        self.assertEqual(q.json()["results"][0]["text"], "Paris is in France.")


if __name__ == "__main__":
    unittest.main()
