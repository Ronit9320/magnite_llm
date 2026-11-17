"""Speed and planning fields the Knowledge panel uses to warn before slow indexing.

GET /rag/embedders gives each preset reference speeds (measured on an Apple M1 8 GB), whether it is
this machine's recommended default, and the device it would run on. GET /rag/status reports the speed
measured on real batched ingests/reindexes (in memory only) and the chunk-size limit for the model.
No model is ever loaded: the embedder is mocked.
"""
import os
import tempfile
import time
import unittest
from unittest.mock import patch

import httpx
import numpy as np

from backend.rag import chunking, embeddings, memguard
from backend.routes import rag
from backend.server import app

MINILM, BGE, QWEN = embeddings.MINILM, embeddings.BGE_M3, embeddings.QWEN3
TEXT = " ".join(f"Sentence number {i} is about Paris and France." for i in range(40))


class SpeedHintTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        env = patch.dict(os.environ, {"MAGNITE_HOME": self.temp.name, "MAGNITE_RAG_BATCH": "4"})
        env.start()
        self.addCleanup(env.stop)
        rag._rag_setup()
        self.store = rag._get_store()
        self.loaded = {"model": None}
        self.fail_embed = False
        dims = {MINILM: 384, BGE: 1024, QWEN: 1024}

        def embed(texts, model, truncate_dim=0):
            if self.fail_embed:
                raise RuntimeError("boom")
            time.sleep(0.002)
            self.loaded["model"] = embeddings.spec_for(model).model_id
            n = dims.get(embeddings.spec_for(model).model_id, 8)
            out = np.stack([np.full(n, float(i + 1)) for i in range(len(texts))]).astype(np.float32)
            return out / np.linalg.norm(out, axis=1, keepdims=True)

        def loaded_info():
            if self.loaded["model"] is None:
                return None
            return {"model": self.loaded["model"], "max_seq_tokens": 256, "dim": 384,
                    "device": "cpu", "dtype": "float32"}

        for p in (patch.object(embeddings, "is_available", return_value=True),
                  patch.object(embeddings, "embed_documents", side_effect=embed),
                  patch.object(embeddings, "loaded_info", side_effect=loaded_info),
                  patch.object(memguard, "available_memory_mb", return_value=50_000.0)):
            p.start()
            self.addCleanup(p.stop)
        embeddings._reset_for_tests()
        self.addCleanup(embeddings._reset_for_tests)
        self.client = httpx.AsyncClient(transport=httpx.ASGITransport(app=app, raise_app_exceptions=False),
                                        base_url="http://test")
        self.addAsyncCleanup(self.client.aclose)

    async def status(self):
        r = await self.client.get("/rag/status")
        self.assertEqual(r.status_code, 200, r.text)
        return r.json()

    async def listing(self):
        r = await self.client.get("/rag/embedders")
        self.assertEqual(r.status_code, 200, r.text)
        return {e["model_id"]: e for e in r.json()}

    # ---- GET /rag/embedders ------------------------------------------------------------------

    async def test_embedders_list_reference_speeds_measured_on_an_m1(self):
        listing = await self.listing()
        self.assertEqual(listing[MINILM]["reference_chunks_per_s"], {"cpu": 40.0})
        self.assertEqual(listing[BGE]["reference_chunks_per_s"], {"cpu": 0.2, "gpu": 2.0})
        self.assertEqual(listing[QWEN]["reference_chunks_per_s"], {"cpu": 0.2, "gpu": 2.0})
        for entry in listing.values():
            self.assertEqual(entry["reference_hardware"], "Apple M1, 8 GB")

    async def test_recommended_flag_marks_only_this_machines_default(self):
        for default in (MINILM, BGE):
            with patch.object(embeddings, "recommended_model", return_value=default):
                listing = await self.listing()
            self.assertEqual([m for m, e in listing.items() if e["recommended"]], [default])

    async def test_expected_device_is_reported_for_each_preset(self):
        listing = await self.listing()
        for entry in listing.values():
            self.assertIn(entry["expected_device"], ("cpu", "gpu"))

    def test_planned_device_follows_the_device_policy_without_torch(self):
        light, heavy = embeddings.PRESETS[MINILM], embeddings.PRESETS[BGE]
        plan = embeddings.planned_device
        self.assertEqual(plan(heavy, total_mb=8192, cuda=False, apple_gpu=True, env={}), "cpu")
        self.assertEqual(plan(heavy, total_mb=32000, cuda=False, apple_gpu=True, env={}), "gpu")
        self.assertEqual(plan(light, total_mb=32000, cuda=False, apple_gpu=True, env={}), "cpu")
        self.assertEqual(plan(light, total_mb=8192, cuda=True, apple_gpu=False, env={}), "gpu")
        self.assertEqual(plan(heavy, total_mb=32000, cuda=True, apple_gpu=False,
                              env={"MAGNITE_RAG_DEVICE": "cpu"}), "cpu")
        self.assertEqual(plan(light, total_mb=8192, cuda=False, apple_gpu=False,
                              env={"MAGNITE_RAG_DEVICE": "mps"}), "gpu")

    # ---- GET /rag/status: measured speed -------------------------------------------------------

    async def test_measured_speed_is_null_until_an_ingest_has_run(self):
        self.assertIsNone((await self.status())["embedder"]["measured_chunks_per_s"])

    async def test_batched_ingest_records_the_measured_speed(self):
        await self.client.post("/rag/config", json={"model": MINILM})
        self.loaded["model"] = MINILM                       # model already loaded: every batch counts
        r = await self.client.post("/rag/documents", json={"name": "a.txt", "content": TEXT,
                                                           "chunk_size": 64, "chunk_overlap": 8})
        self.assertEqual(r.status_code, 201, r.text)
        self.assertGreater(r.json()["chunks"], 4)
        speed = (await self.status())["embedder"]["measured_chunks_per_s"]
        self.assertIsNotNone(speed)
        self.assertGreater(speed, 0)

    async def test_the_batch_that_loads_the_model_is_not_timed(self):
        await self.client.post("/rag/config", json={"model": MINILM})
        r = await self.client.post("/rag/documents", json={"name": "a.txt", "content": "Paris is in France."})
        self.assertEqual(r.status_code, 201, r.text)          # one batch, and it loaded the model
        self.assertIsNone((await self.status())["embedder"]["measured_chunks_per_s"])

    async def test_failed_ingest_records_nothing(self):
        await self.client.post("/rag/config", json={"model": MINILM})
        self.loaded["model"] = MINILM
        self.fail_embed = True
        r = await self.client.post("/rag/documents", json={"name": "a.txt", "content": TEXT})
        self.assertEqual(r.status_code, 503, r.text)
        self.assertIsNone((await self.status())["embedder"]["measured_chunks_per_s"])

    async def test_reindex_records_the_measured_speed_for_the_new_model(self):
        await self.client.post("/rag/config", json={"model": MINILM})
        r = await self.client.post("/rag/documents", json={"name": "a.txt", "content": TEXT,
                                                           "chunk_size": 64, "chunk_overlap": 8})
        self.assertEqual(r.status_code, 201, r.text)
        embeddings._reset_measured_for_tests()
        self.loaded["model"] = BGE
        r = await self.client.post("/rag/reindex", json={"model": BGE})
        self.assertEqual(r.status_code, 200, r.text)
        data = await self.status()
        self.assertEqual(data["model"], BGE)
        self.assertGreater(data["embedder"]["measured_chunks_per_s"] or 0, 0)

    async def test_measured_speed_belongs_to_its_model(self):
        embeddings.record_speed(BGE, chunks=100, seconds=50.0, device="cpu", dtype="float32")
        await self.client.post("/rag/config", json={"model": MINILM})
        self.assertIsNone((await self.status())["embedder"]["measured_chunks_per_s"])
        self.assertAlmostEqual(embeddings.measured_speed(BGE), 2.0)

    # ---- GET /rag/status: chunk-size limit -----------------------------------------------------

    async def test_status_reports_the_chunk_size_limit_for_the_model(self):
        await self.client.post("/rag/config", json={"model": MINILM, "chunk_size": 512, "chunk_overlap": 64})
        data = await self.status()
        self.assertEqual(data["chunking"]["max_chunk_size"], chunking.chunk_limit(256))
        self.assertEqual(data["chunking"]["chunk_size"], data["chunking"]["max_chunk_size"])
        await self.client.post("/rag/config", json={"model": BGE})
        data = await self.status()
        self.assertEqual(data["chunking"]["max_chunk_size"], chunking.chunk_limit(embeddings.MAX_SEQ_TOKENS_CAP))
        self.assertLessEqual(data["chunking"]["chunk_size"], data["chunking"]["max_chunk_size"])


if __name__ == "__main__":
    unittest.main()
