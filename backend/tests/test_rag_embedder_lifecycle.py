"""Embedder registry lifecycle: a model in use is never unloaded, and a long run can pin its model.

Break report 2026-11-20, F1 (idle timer fired inside one long embed call) and F2 (a query during a
reindex swapped the single resident model back and forth).
"""
import gc
import os
import shutil
import tempfile
import time
import unittest
from collections import Counter
from unittest.mock import patch

import numpy as np

from backend.rag import embeddings, memguard
from backend.rag.memguard import MemoryGuardTripped
from backend.rag.schemas import RagReindexRequest
from backend.rag.store import RagStore, get_rag_store, set_rag_store
from backend.routes import rag

from test_rag_embedder import BGE, MINILM, EmbedderTestCase, FakeEncoder


class UseGuardsTheModel(EmbedderTestCase):
    def test_idle_timer_firing_mid_call_does_not_unload_the_model(self):
        """F1: BGE-M3 on a CPU takes ~320 s for one reindex batch; the default idle limit is 300 s."""
        emb = embeddings.get_embedder(MINILM)
        original, seen = FakeEncoder.encode, {"batches": 0, "unloaded": []}

        def encode(encoder, texts, **kw):
            seen["batches"] += 1
            if seen["batches"] == 2:                      # the idle timer fires while this call is running
                seen["unloaded"].append(embeddings.unload_if_idle(now=time.monotonic() + 10 ** 6))
            return original(encoder, texts, **kw)

        with patch.object(FakeEncoder, "encode", encode):
            emb.embed_documents([f"t{i}" for i in range(40)], batch_size=8)
        self.assertEqual(seen["unloaded"], [False])
        self.assertEqual(len(self.loads), 1)              # never reloaded
        self.assertIsNotNone(embeddings.loaded_info())    # still registered for the next call

    def test_a_model_that_finished_its_call_still_idles_out(self):
        emb = embeddings.get_embedder(MINILM)
        emb.embed_documents(["a", "b"])
        self.assertTrue(embeddings.unload_if_idle(now=time.monotonic() + 10 ** 6))
        self.assertIsNone(embeddings.loaded_info())

    def test_a_failed_call_releases_the_use_guard(self):
        emb = embeddings.get_embedder(MINILM)
        with patch.object(FakeEncoder, "encode", side_effect=RuntimeError("boom")):
            with self.assertRaises(RuntimeError):
                emb.embed_query("q")
        self.assertTrue(embeddings.unload_if_idle(now=time.monotonic() + 10 ** 6))


class PinnedModelTests(EmbedderTestCase):
    def peak_alive(self):
        gc.collect()
        return len(FakeEncoder.live)

    def test_queries_on_the_old_model_do_not_evict_the_pinned_one(self):
        """F2: during a reindex the old model keeps answering queries; neither side reloads."""
        embeddings.get_embedder(MINILM)
        with embeddings.pinned(BGE):
            for _ in range(5):
                embeddings.embed_documents(["a"], BGE)           # a reindex batch
                embeddings.embed_query("q", MINILM)              # a chat query on the old model
        self.assertEqual(Counter(load[0] for load in self.loads), {MINILM: 1, BGE: 1})

    def test_both_models_are_resident_only_while_pinned(self):
        embeddings.get_embedder(MINILM)
        with embeddings.pinned(BGE):
            embeddings.embed_documents(["a"], BGE)
            self.assertEqual(self.peak_alive(), 2)
        self.encoders.clear()
        self.assertEqual(self.peak_alive(), 1)

    def test_release_promotes_the_pinned_model_so_it_is_not_reloaded(self):
        embeddings.get_embedder(MINILM)
        with embeddings.pinned(BGE):
            embeddings.embed_documents(["a"], BGE)
        self.assertEqual(embeddings.loaded_info()["model"], BGE)
        self.assertIsNone(embeddings.loaded_info(MINILM))
        embeddings.embed_query("q", BGE)
        self.assertEqual(Counter(load[0] for load in self.loads)[BGE], 1)

    def test_pinning_the_model_that_is_already_resident_reuses_it(self):
        embeddings.get_embedder(MINILM)
        with embeddings.pinned(MINILM):
            embeddings.embed_documents(["a"], MINILM)
        self.assertEqual(len(self.loads), 1)

    def test_pin_alone_loads_nothing(self):
        with embeddings.pinned(BGE):
            pass
        self.assertEqual(self.loads, [])

    def test_loading_the_other_model_is_preflighted_while_pinned(self):
        with embeddings.pinned(BGE):
            embeddings.embed_documents(["a"], BGE)
            with patch.object(memguard, "available_memory_mb", return_value=300.0):
                with self.assertRaises(MemoryGuardTripped):
                    embeddings.embed_query("q", MINILM)
            self.assertIsNotNone(embeddings.loaded_info(BGE))     # the pinned model was left alone

    def test_lookups_see_the_pinned_model(self):
        with embeddings.pinned(BGE):
            embeddings.embed_documents(["a"], BGE)
            self.assertEqual(embeddings.loaded_info(BGE)["model"], BGE)
            self.assertTrue(embeddings.status_for(BGE)["loaded"])
            self.assertIsNotNone(embeddings.count_tokens(["one two"], BGE))

    def test_idle_unload_never_unloads_the_pinned_model(self):
        with embeddings.pinned(BGE):
            embeddings.embed_documents(["a"], BGE)
            embeddings.unload_if_idle(now=time.monotonic() + 10 ** 6)
            self.assertIsNotNone(embeddings.loaded_info(BGE))

    def test_pin_is_released_when_the_body_raises(self):
        with self.assertRaises(RuntimeError):
            with embeddings.pinned(BGE):
                embeddings.embed_documents(["a"], BGE)
                raise RuntimeError("cancelled")
        self.encoders.clear()
        embeddings.get_embedder(MINILM)                           # normal one-model switching is back
        self.encoders.clear()
        self.assertEqual(self.peak_alive(), 1)


class ReindexPinsItsModel(EmbedderTestCase):
    def setUp(self):
        super().setUp()
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        previous = get_rag_store()
        self.addCleanup(set_rag_store, previous) if previous is not None else None
        self.store = RagStore(self.dir)
        set_rag_store(self.store)
        self.store.update_config({"model": MINILM})
        n = 130                                                   # three reindex batches of 64
        vectors = np.ones((n, 16), np.float32) / 4.0
        self.store.add_document("d", "txt", "", MINILM, [f"chunk {i} words" for i in range(n)], vectors, 1.0, 64, 8)

    def test_queries_during_a_reindex_cause_no_reloads_and_the_speed_is_still_measured(self):
        embeddings.get_embedder(MINILM)
        original, queries = FakeEncoder.encode, []

        def encode(encoder, texts, **kw):
            if len(texts) > 1 and len(queries) < 3:               # a reindex batch is running: a chat query arrives
                queries.append(embeddings.embed_query("question", MINILM).shape)
            return original(encoder, texts, **kw)

        with patch.object(FakeEncoder, "encode", encode):
            rag._reindex(RagReindexRequest(model=BGE), None)
        self.assertEqual(len(queries), 3)
        self.assertEqual(Counter(load[0] for load in self.loads), {MINILM: 1, BGE: 1})
        self.assertEqual(self.store.get_config().model, BGE)
        self.assertIsNotNone(embeddings.measured_speed(BGE))      # the speed meter can see the pinned model


if __name__ == "__main__":
    unittest.main()
