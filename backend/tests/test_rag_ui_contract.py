"""The UI must always be able to show the parameters the backend is really using.

Whatever embedder is configured (or switched to), GET /rag/status reports the effective embedder
limits and chunking, and it must agree with GET /rag/config. The desktop Knowledge panel is expected to
display `status.chunking` / `status.embedder` rather than hardcoded or stored-only values.
"""
import os
import tempfile
import unittest
from unittest.mock import patch

import httpx
import numpy as np

from backend.rag import embeddings, memguard
from backend.routes import rag
from backend.server import app

MINILM, BGE, QWEN = embeddings.MINILM, embeddings.BGE_M3, embeddings.QWEN3
CONFIG_KEYS_THE_CLIENT_ALREADY_READS = {"chunk_size", "chunk_overlap", "top_k", "min_score", "model"}


class UiContractTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        env = patch.dict(os.environ, {"MAGNITE_HOME": self.temp.name})
        env.start()
        self.addCleanup(env.stop)
        rag._rag_setup()
        self.store = rag._get_store()
        dims = {MINILM: 384, BGE: 1024, QWEN: 1024}

        def embed(texts, model, truncate_dim=0):
            n = dims.get(embeddings.spec_for(model).model_id, 8)
            out = np.stack([np.full(n, float(i + 1)) for i in range(len(texts))]).astype(np.float32)
            return out / np.linalg.norm(out, axis=1, keepdims=True)

        for p in (patch.object(embeddings, "is_available", return_value=True),
                  patch.object(embeddings, "embed_documents", side_effect=embed),
                  patch.object(embeddings, "embed_query", side_effect=lambda t, m, d=0: embed([t], m)[0]),
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

    async def test_status_config_and_get_config_always_agree(self):
        for model in (MINILM, BGE, QWEN):
            await self.client.post("/rag/config", json={"model": model})
            status, config = await self.status(), (await self.client.get("/rag/config")).json()
            self.assertEqual(status["config"], config, model)
            self.assertEqual(status["model"], config["model"])
            self.assertEqual(status["embedder"]["model"], config["model"])

    async def test_effective_chunking_follows_the_model_when_it_changes(self):
        await self.client.post("/rag/config", json={"model": BGE, "chunk_size": 512, "chunk_overlap": 64})
        self.assertEqual((await self.status())["chunking"], {"chunk_size": 512, "chunk_overlap": 64, "clamped": False,
                                                            "max_chunk_size": 601})
        await self.client.post("/rag/config", json={"model": MINILM})
        data = await self.status()
        self.assertEqual(data["chunking"], {"chunk_size": 149, "chunk_overlap": 37, "clamped": True,
                                            "max_chunk_size": 149})
        self.assertEqual(data["embedder"]["max_seq_tokens"], 256)
        self.assertEqual(data["embedder"]["dim"], 384)
        self.assertEqual(data["config"]["chunk_size"], 512)          # the stored value is untouched; "effective" is separate

    async def test_status_follows_a_reindex_to_another_model(self):
        await self.client.post("/rag/config", json={"model": MINILM})
        r = await self.client.post("/rag/documents", json={"name": "a.txt", "content": "Paris is in France."})
        self.assertEqual(r.status_code, 201, r.text)
        before = await self.status()
        self.assertEqual((before["embedder"]["label"], before["embedder"]["dim"]), ("Light", 384))
        r = await self.client.post("/rag/reindex", json={"model": BGE})
        self.assertEqual(r.status_code, 200, r.text)
        after = await self.status()
        self.assertEqual((after["model"], after["embedder"]["label"], after["embedder"]["dim"]), (BGE, "Quality", 1024))
        self.assertEqual(after["embedder"]["max_seq_tokens"], 1024)       # capped long-context limit
        self.assertEqual(after["config"], (await self.client.get("/rag/config")).json())
        self.assertEqual(after["documents"], 1)

    async def test_truncate_dim_is_reflected_in_the_reported_dimension(self):
        await self.client.post("/rag/config", json={"model": QWEN, "truncate_dim": 512})
        data = await self.status()
        self.assertEqual((data["embedder"]["dim"], data["config"]["truncate_dim"]), (512, 512))

    async def test_existing_config_keys_the_client_reads_are_still_present(self):
        config = (await self.client.get("/rag/config")).json()
        self.assertTrue(CONFIG_KEYS_THE_CLIENT_ALREADY_READS <= set(config))
        status = await self.status()
        for key in ("engine_ready", "model", "documents", "chunks", "config"):
            self.assertIn(key, status)

    async def test_embedders_listing_matches_what_status_reports_for_each_preset(self):
        listing = {e["model_id"]: e for e in (await self.client.get("/rag/embedders")).json()}
        for model, entry in listing.items():
            await self.client.post("/rag/config", json={"model": model})
            reported = (await self.status())["embedder"]
            self.assertEqual(reported["dim"], entry["dim"], model)
            self.assertEqual(reported["max_seq_tokens"], min(entry["max_seq_tokens"], embeddings.MAX_SEQ_TOKENS_CAP))


if __name__ == "__main__":
    unittest.main()
