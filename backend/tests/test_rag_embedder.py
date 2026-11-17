import gc
import os
import shutil
import tempfile
import types
import unittest
import weakref
import zlib
from unittest.mock import patch

import httpx
import numpy as np

from backend.rag import embeddings, memguard
from backend.rag.embeddings import Embedder, EmbedderSpec, MAX_SEQ_TOKENS_CAP, PRESETS
from backend.rag.memguard import MemoryGuardTripped
from backend.rag.schemas import RagConfig
from backend.rag.store import RagStore
from backend.routes import rag
from backend.server import app

QWEN = "Qwen/Qwen3-Embedding-0.6B"
BGE = "BAAI/bge-m3"
MINILM = "sentence-transformers/all-MiniLM-L6-v2"
SHORT_MINILM = "all-MiniLM-L6-v2"      # the id RagConfig has always defaulted to


class FakeEncoder:
    """Stands in for a SentenceTransformer: deterministic unit vectors, records every call."""
    live = weakref.WeakSet()

    def __init__(self, dim=16, max_seq_length=256):
        self.dim = dim
        self.max_seq_length = max_seq_length
        self.calls = []
        self.tokenizer = lambda texts, **kw: {"input_ids": [[0] * (2 + 2 * len(t.split())) for t in texts]}
        FakeEncoder.live.add(self)

    def encode(self, texts, batch_size=32, normalize_embeddings=True, convert_to_numpy=True,
               show_progress_bar=False, **kw):
        self.calls.append(list(texts))
        out = np.stack([np.random.default_rng(zlib.crc32(t.encode())).standard_normal(self.dim) for t in texts])
        out = out.astype(np.float32)
        if normalize_embeddings:
            out /= np.linalg.norm(out, axis=1, keepdims=True)
        return out


class EmbedderTestCase(unittest.TestCase):
    def setUp(self):
        self.loads = []                 # (spec.model_id, device, dtype, live encoders at load time)
        self.encoders = []
        gc.collect()

        def loader(spec, device, dtype):
            self.loads.append((spec.model_id, device, dtype, len(FakeEncoder.live)))
            enc = FakeEncoder(dim=spec.dim or 16, max_seq_length=spec.max_seq_tokens or 256)
            self.encoders.append(enc)
            return enc

        for p in (patch.object(embeddings, "_load_encoder", side_effect=loader),
                  patch.object(embeddings, "_import_torch", return_value=None),
                  patch.object(memguard, "available_memory_mb", return_value=50_000.0),
                  patch.object(memguard, "total_memory_mb", return_value=8192.0)):
            p.start()
            self.addCleanup(p.stop)
        self.addCleanup(embeddings._reset_for_tests)
        embeddings._reset_for_tests()


class PromptTests(EmbedderTestCase):
    def test_qwen_prefixes_queries_but_not_documents(self):
        emb = embeddings.get_embedder(QWEN)
        emb.embed_documents(["a document"])
        emb.embed_query("a question")
        docs_call, query_call = self.encoders[0].calls
        self.assertEqual(docs_call, ["a document"])
        self.assertTrue(query_call[0].startswith("Instruct: "))
        self.assertTrue(query_call[0].endswith("a question"))

    def test_minilm_and_bge_use_no_prompts(self):
        for model in (MINILM, BGE):
            emb = embeddings.get_embedder(model)
            emb.embed_query("q")
            emb.embed_documents(["d"])
            self.assertEqual(self.encoders[-1].calls, [["q"], ["d"]], model)

    def test_presets_cover_the_three_models_with_labels(self):
        self.assertEqual(PRESETS[MINILM].label, "Light")
        self.assertEqual(PRESETS[BGE].label, "Quality")
        self.assertEqual(PRESETS[QWEN].label, "Alternative")
        self.assertEqual(PRESETS[MINILM].max_seq_tokens, 256)
        self.assertEqual(PRESETS[BGE].dim, 1024)
        self.assertEqual({p.approx_ram_mb for p in PRESETS.values()}, {450, 3000})

    def test_short_minilm_id_resolves_to_the_preset(self):
        self.assertEqual(embeddings.spec_for(SHORT_MINILM), PRESETS[MINILM])

    def test_unknown_model_gets_a_generic_spec(self):
        spec = embeddings.spec_for("someone/else-embed")
        self.assertEqual(spec.model_id, "someone/else-embed")
        self.assertEqual((spec.query_prompt, spec.document_prompt), ("", ""))


class EncodingTests(EmbedderTestCase):
    def test_documents_are_embedded_in_ordered_batches(self):
        emb = embeddings.get_embedder(MINILM)
        texts = [f"text {i}" for i in range(20)]
        vecs = emb.embed_documents(texts, batch_size=8)
        self.assertEqual(vecs.shape, (20, 384))
        self.assertEqual([len(c) for c in self.encoders[0].calls], [8, 8, 4])
        self.assertEqual(sum(self.encoders[0].calls, []), texts)
        again = self.encoders[0].encode(["text 13"])
        np.testing.assert_allclose(vecs[13], again[0], atol=1e-6)

    def test_vectors_are_float32_and_unit_length(self):
        vecs = embeddings.get_embedder(MINILM).embed_documents(["a", "b", "c"])
        self.assertEqual(vecs.dtype, np.float32)
        np.testing.assert_allclose(np.linalg.norm(vecs, axis=1), 1.0, atol=1e-5)

    def test_embed_query_is_one_dimensional(self):
        self.assertEqual(embeddings.get_embedder(MINILM).embed_query("q").shape, (384,))

    def test_empty_input_returns_empty_matrix(self):
        self.assertEqual(embeddings.get_embedder(MINILM).embed_documents([]).shape[0], 0)

    def test_empty_string_does_not_produce_nan(self):
        emb = embeddings.get_embedder(MINILM)
        with patch.object(FakeEncoder, "encode", return_value=np.zeros((1, 384), np.float32)):
            vec = emb.embed_documents([""])
        self.assertFalse(np.isnan(vec).any())

    def test_truncate_dim_slices_and_renormalizes(self):
        emb = embeddings.get_embedder(QWEN, truncate_dim=512)
        vecs = emb.embed_documents(["alpha", "beta"])
        self.assertEqual(vecs.shape, (2, 512))
        np.testing.assert_allclose(np.linalg.norm(vecs, axis=1), 1.0, atol=1e-5)
        self.assertEqual(emb.dim, 512)
        self.assertEqual(emb.embed_query("q").shape, (512,))

    def test_truncate_dim_larger_than_native_is_a_noop(self):
        emb = embeddings.get_embedder(QWEN, truncate_dim=4096)
        self.assertEqual(emb.embed_documents(["x"]).shape, (1, 1024))

    def test_truncation_is_refused_for_models_that_do_not_support_it(self):
        with self.assertRaises(ValueError) as ctx:
            embeddings.get_embedder(MINILM, truncate_dim=128)
        self.assertIn("truncat", str(ctx.exception).lower())

    def test_changing_truncate_dim_keeps_the_loaded_model(self):
        embeddings.get_embedder(QWEN, truncate_dim=0)
        emb = embeddings.get_embedder(QWEN, truncate_dim=256)
        self.assertEqual(len(self.loads), 1)
        self.assertEqual(emb.embed_documents(["x"]).shape, (1, 256))

    def test_count_tokens_uses_the_tokenizer_and_falls_back_to_words(self):
        emb = embeddings.get_embedder(MINILM)
        self.assertEqual(emb.count_tokens(["one two three"]), [8])
        self.encoders[0].tokenizer = None
        self.assertEqual(emb.count_tokens(["one two three"]), [3])

    def test_max_seq_tokens_is_capped(self):
        self.assertEqual(embeddings.get_embedder(QWEN).max_seq_tokens, MAX_SEQ_TOKENS_CAP)
        self.assertEqual(embeddings.get_embedder(MINILM).max_seq_tokens, 256)

    def test_legacy_functions_still_work(self):
        self.assertEqual(embeddings.embed_texts(["a", "b"], MINILM).shape, (2, 384))
        self.assertEqual(embeddings.dimension(MINILM), 384)
        self.assertEqual(embeddings.embed_query("q", MINILM).shape, (384,))
        self.assertEqual(embeddings.embed_documents(["a"], MINILM).shape, (1, 384))


class SelectionPolicyTests(unittest.TestCase):
    def test_default_model_by_ram_and_cuda(self):
        self.assertEqual(embeddings.default_model(8192, False), MINILM)
        self.assertEqual(embeddings.default_model(16384, False), BGE)
        self.assertEqual(embeddings.default_model(15800, False), BGE)       # a "16 GB" Linux box
        self.assertEqual(embeddings.default_model(8192, True), BGE)

    def torch_stub(self, cuda=False, mps=False, bf16=True):
        return types.SimpleNamespace(
            cuda=types.SimpleNamespace(is_available=lambda: cuda, is_bf16_supported=lambda: bf16),
            backends=types.SimpleNamespace(mps=types.SimpleNamespace(is_available=lambda: mps)))

    def test_pick_device_policy(self):
        big, small = PRESETS[BGE], PRESETS[MINILM]
        self.assertEqual(embeddings.pick_device(big, 8192, self.torch_stub(mps=True), env={}), "cpu")
        self.assertEqual(embeddings.pick_device(small, 32768, self.torch_stub(mps=True), env={}), "cpu")
        self.assertEqual(embeddings.pick_device(big, 32768, self.torch_stub(mps=True), env={}), "mps")
        self.assertEqual(embeddings.pick_device(small, 8192, self.torch_stub(cuda=True), env={}), "cuda")
        self.assertEqual(embeddings.pick_device(big, 8192, None, env={}), "cpu")
        self.assertEqual(embeddings.pick_device(big, 32768, self.torch_stub(mps=True),
                                                env={"MAGNITE_RAG_DEVICE": "cpu"}), "cpu")

    def test_pick_dtype_per_device(self):
        self.assertEqual(embeddings.pick_dtype("cuda", self.torch_stub(cuda=True, bf16=True), env={}), "bfloat16")
        self.assertEqual(embeddings.pick_dtype("cuda", self.torch_stub(cuda=True, bf16=False), env={}), "float16")
        self.assertEqual(embeddings.pick_dtype("mps", self.torch_stub(mps=True), env={}), "float16")
        self.assertEqual(embeddings.pick_dtype("cpu", self.torch_stub(), env={}), "float32")
        self.assertEqual(embeddings.pick_dtype("cpu", None, env={}), "float32")
        self.assertEqual(embeddings.pick_dtype("cpu", None, env={"MAGNITE_RAG_DTYPE": "bfloat16"}), "bfloat16")
        self.assertEqual(embeddings.pick_dtype("cpu", None, env={"MAGNITE_RAG_DTYPE": "nonsense"}), "float32")


class OneModelAtATimeTests(EmbedderTestCase):
    def test_switching_models_never_holds_two(self):
        for model in (MINILM, BGE, MINILM):
            embeddings.get_embedder(model)
            self.encoders.clear()          # the test must not keep encoders alive itself
        self.assertEqual([load[0] for load in self.loads], [MINILM, BGE, MINILM])
        self.assertEqual([load[3] for load in self.loads], [0, 0, 0])      # nothing else alive at each load

    def test_same_model_is_loaded_once(self):
        a = embeddings.get_embedder(MINILM)
        b = embeddings.get_embedder(MINILM)
        self.assertIs(a, b)
        self.assertEqual(len(self.loads), 1)

    def test_load_uses_the_policy_device_and_dtype(self):
        embeddings.get_embedder(MINILM)
        self.assertEqual(self.loads[0][1:3], ("cpu", "float32"))

    def test_preflight_refuses_load_when_memory_is_low(self):
        with patch.object(memguard, "available_memory_mb", return_value=500.0):
            with self.assertRaises(MemoryGuardTripped):
                embeddings.get_embedder(BGE)                                # needs ~3000 MB x 1.5
        self.assertEqual(self.loads, [])
        embeddings.get_embedder(MINILM)                                      # a failed load leaves the registry usable
        self.assertEqual(len(self.loads), 1)

    def test_a_failed_switch_does_not_leave_a_half_loaded_embedder(self):
        embeddings.get_embedder(MINILM)
        with patch.object(memguard, "available_memory_mb", return_value=500.0):
            with self.assertRaises(MemoryGuardTripped):
                embeddings.get_embedder(BGE)
        self.assertEqual(embeddings.loaded_info(), None)

    def test_guard_trip_mid_embedding_stops_further_batches(self):
        emb = embeddings.get_embedder(MINILM)
        checks = {"n": 0}

        class TripSecond:
            peak_footprint_mb = 0.0

            def check(self_inner):
                checks["n"] += 1
                if checks["n"] == 2:
                    raise MemoryGuardTripped("Embedding stopped: test")

        emb._guard_factory = lambda: TripSecond()
        with self.assertRaises(MemoryGuardTripped):
            emb.embed_documents([f"t{i}" for i in range(40)], batch_size=8)
        self.assertEqual(len(self.encoders[0].calls), 2)                     # batch 3 never ran

    def test_accelerator_cache_is_released_between_batches(self):
        emb = embeddings.get_embedder(MINILM)
        with patch.object(memguard, "release_accelerator_memory") as release:
            emb.embed_documents([f"t{i}" for i in range(40)], batch_size=8)
        self.assertGreaterEqual(release.call_count, 1)


class IdleUnloadTests(EmbedderTestCase):
    def test_idle_unload_frees_and_reloads(self):
        with patch.object(embeddings.time, "monotonic", return_value=1000.0):
            embeddings.get_embedder(MINILM)
        self.assertFalse(embeddings.unload_if_idle(now=1000.0 + 299))
        self.assertIsNotNone(embeddings.loaded_info())
        self.assertTrue(embeddings.unload_if_idle(now=1000.0 + 301))
        self.assertIsNone(embeddings.loaded_info())
        ref = weakref.ref(self.encoders[0])
        self.encoders.clear()
        gc.collect()
        self.assertIsNone(ref())                                              # the encoder was actually freed
        embeddings.get_embedder(MINILM)                                       # transparent reload
        self.assertEqual(len(self.loads), 2)

    def test_use_resets_the_idle_clock(self):
        with patch.object(embeddings.time, "monotonic", return_value=1000.0):
            emb = embeddings.get_embedder(MINILM)
        with patch.object(embeddings.time, "monotonic", return_value=1250.0):
            emb.embed_query("q")
        self.assertFalse(embeddings.unload_if_idle(now=1250.0 + 299))
        self.assertTrue(embeddings.unload_if_idle(now=1250.0 + 301))

    def test_idle_timeout_env_and_disable(self):
        with patch.dict(os.environ, {"MAGNITE_RAG_IDLE_UNLOAD_S": "10"}):
            with patch.object(embeddings.time, "monotonic", return_value=0.0):
                embeddings.get_embedder(MINILM)
            self.assertTrue(embeddings.unload_if_idle(now=11.0))
        with patch.dict(os.environ, {"MAGNITE_RAG_IDLE_UNLOAD_S": "0"}):
            embeddings.get_embedder(MINILM)
            self.assertFalse(embeddings.unload_if_idle(now=10 ** 9))

    def test_unload_when_nothing_is_loaded_is_false(self):
        self.assertFalse(embeddings.unload_if_idle(now=10 ** 9))


class StoreConfigTests(EmbedderTestCase):
    def setUp(self):
        super().setUp()
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.store = RagStore(self.dir)

    def add(self, model, dim=4):
        self.store.update_config({"model": model}) if self.store.count_chunks() == 0 else None
        return self.store.add_document("d", "txt", "", model, ["chunk"], np.ones((1, dim), np.float32), 1.0, 64, 8)

    def test_default_model_follows_existing_documents(self):
        """A library built with MiniLM must stay searchable when the recommended default changes."""
        self.add(SHORT_MINILM)
        with self.store._conn() as conn:                       # simulate a pre-upgrade store: no saved config
            conn.execute("DELETE FROM rag_config")
            conn.commit()
        with patch.object(embeddings, "recommended_model", return_value=BGE):
            self.assertEqual(self.store.get_config().model, SHORT_MINILM)
            res = self.store.search(np.ones(4, np.float32) / 2, self.store.get_config().model)
        self.assertEqual(len(res), 1)

    def test_fresh_store_uses_the_recommended_model(self):
        with patch.object(embeddings, "recommended_model", return_value=BGE):
            self.assertEqual(self.store.get_config().model, BGE)

    def test_saved_model_wins_over_everything(self):
        self.store.update_config({"model": QWEN})
        with patch.object(embeddings, "recommended_model", return_value=BGE):
            self.assertEqual(self.store.get_config().model, QWEN)

    def test_truncate_dim_is_stored_and_defaults_to_zero(self):
        self.assertEqual(RagConfig().truncate_dim, 0)
        self.store.update_config({"model": QWEN, "truncate_dim": 512})
        self.assertEqual(self.store.get_config().truncate_dim, 512)

    def test_truncate_dim_change_with_documents_is_blocked(self):
        self.add(SHORT_MINILM)
        with self.assertRaises(ValueError) as ctx:
            self.store.update_config({"truncate_dim": 256})
        self.assertIn("re-ingest", str(ctx.exception))
        self.assertEqual(self.store.get_config().truncate_dim, 0)

    def test_truncate_dim_same_value_with_documents_is_allowed(self):
        self.add(SHORT_MINILM)
        self.store.update_config({"truncate_dim": 0, "top_k": 7})
        self.assertEqual(self.store.get_config().top_k, 7)


class RouteTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        env = patch.dict(os.environ, {"MAGNITE_HOME": self.temp.name})
        env.start()
        self.addCleanup(env.stop)
        rag._rag_setup()
        self.store = rag._get_store()
        self.store.update_config({"model": SHORT_MINILM})
        for p in (patch.object(embeddings, "_load_encoder",
                               side_effect=lambda spec, d, t: FakeEncoder(dim=spec.dim or 16,
                                                                          max_seq_length=spec.max_seq_tokens or 256)),
                  patch.object(embeddings, "_import_torch", return_value=None),
                  patch.object(embeddings, "is_available", return_value=True),
                  patch.object(memguard, "available_memory_mb", return_value=50_000.0),
                  patch.object(memguard, "total_memory_mb", return_value=8192.0)):
            p.start()
            self.addCleanup(p.stop)
        self.addCleanup(embeddings._reset_for_tests)
        embeddings._reset_for_tests()
        self.client = httpx.AsyncClient(transport=httpx.ASGITransport(app=app, raise_app_exceptions=False),
                                        base_url="http://test")
        self.addAsyncCleanup(self.client.aclose)

    async def test_embedders_route_lists_presets(self):
        data = (await self.client.get("/rag/embedders")).json()
        by_id = {e["model_id"]: e for e in data}
        self.assertEqual(set(by_id), {MINILM, BGE, QWEN})
        self.assertEqual(by_id[BGE]["label"], "Quality")
        for key in ("max_seq_tokens", "dim", "approx_ram_mb", "supports_truncation"):
            self.assertIn(key, by_id[BGE])

    async def test_status_reports_embedder_and_effective_chunking(self):
        self.store.update_config({"chunk_size": 512, "chunk_overlap": 64})
        data = (await self.client.get("/rag/status")).json()
        self.assertEqual(data["embedder"]["model"], SHORT_MINILM)
        self.assertEqual(data["embedder"]["max_seq_tokens"], 256)
        self.assertFalse(data["embedder"]["loaded"])                         # status never loads a model
        self.assertEqual(data["chunking"], {"chunk_size": 149, "chunk_overlap": 37, "clamped": True})
        self.assertIn("config", data)                                        # existing keys untouched

    async def test_status_not_clamped_for_a_long_context_model(self):
        self.store.update_config({"model": BGE, "chunk_size": 320, "chunk_overlap": 40})
        data = (await self.client.get("/rag/status")).json()
        self.assertEqual(data["chunking"], {"chunk_size": 320, "chunk_overlap": 40, "clamped": False})

    async def test_ingest_clamps_chunks_to_the_model_limit(self):
        self.store.update_config({"chunk_size": 512, "chunk_overlap": 64})
        text = ". ".join(" ".join(f"w{s}x{i}" for i in range(15)) for s in range(200)) + "."
        response = await self.client.post("/rag/documents", json={"name": "long.txt", "content": text})
        self.assertEqual(response.status_code, 201, response.text)
        res = await self.client.post("/rag/query", json={"query": "w0x0", "top_k": 20})
        sizes = [len(r["text"].split()) for r in res.json()["results"]]
        self.assertTrue(sizes)
        self.assertLessEqual(max(sizes), 149)

    async def test_ingest_warns_when_chunks_exceed_the_token_limit(self):
        text = " ".join(f"word{i}" for i in range(40))
        with self.assertLogs("backend.rag", level="WARNING") as logs:
            with patch.object(Embedder, "count_tokens", return_value=[999]):
                response = await self.client.post("/rag/documents", json={"name": "a.txt", "content": text})
        self.assertEqual(response.status_code, 201, response.text)
        self.assertTrue(any("token limit" in line for line in logs.output))

    def encoder_with_tokens_per_word(self, per_word):
        def loader(spec, device, dtype):
            enc = FakeEncoder(dim=spec.dim or 16, max_seq_length=spec.max_seq_tokens or 256)
            enc.tokenizer = lambda texts, **kw: {"input_ids": [[0] * (2 + per_word * len(t.split())) for t in texts]}
            return enc
        return patch.object(embeddings, "_load_encoder", side_effect=loader)

    async def test_ingest_refits_chunks_using_the_real_token_counts(self):
        """Number-heavy text tokenizes far above the 1.7 tokens/word estimate; chunks must still fit."""
        text = ". ".join(" ".join(f"term{s}x{i}" for i in range(15)) for s in range(400)) + "."
        with self.encoder_with_tokens_per_word(4), self.assertNoLogs("backend.rag", level="WARNING"):
            response = await self.client.post("/rag/documents", json={"name": "num.txt", "content": text})
        self.assertEqual(response.status_code, 201, response.text)
        with self.store._conn() as conn:
            sizes = [len(r["text"].split()) for r in conn.execute("SELECT text FROM rag_chunks")]
        self.assertLessEqual(max(sizes), (256 - 2) // 4)               # 63 words = 254 tokens
        self.assertGreater(len(sizes), 6000 // 63 - 5)                 # and nothing was dropped
        self.assertEqual(sum(1 for _ in sizes), response.json()["chunks"])

    async def test_ingest_does_not_rechunk_when_chunks_already_fit(self):
        text = " ".join(f"word{i}" for i in range(400)) + "."
        with self.encoder_with_tokens_per_word(1):
            response = await self.client.post("/rag/documents", json={"name": "fits.txt", "content": text})
        with self.store._conn() as conn:
            sizes = [len(r["text"].split()) for r in conn.execute("SELECT text FROM rag_chunks")]
        self.assertEqual(max(sizes), 149)                              # the clamp from fit_chunking, untouched

    async def test_refit_gives_up_after_a_few_rounds_and_warns(self):
        text = " ".join(f"word{i}" for i in range(400)) + "."
        with patch.object(Embedder, "count_tokens", side_effect=lambda texts: [999] * len(texts)):
            with self.assertLogs("backend.rag", level="WARNING"):
                response = await self.client.post("/rag/documents", json={"name": "bad.txt", "content": text})
        self.assertEqual(response.status_code, 201, response.text)

    async def test_memory_guard_trip_during_ingest_is_503_with_the_message(self):
        with patch.object(memguard, "available_memory_mb", return_value=10.0):
            response = await self.client.post("/rag/documents", json={"name": "a.txt", "content": "some text"})
        self.assertEqual(response.status_code, 503, response.text)
        self.assertIn("free memory", response.text)
        self.assertEqual(self.store.count_chunks(), 0)

    async def test_query_uses_the_query_prompt_path(self):
        self.store.update_config({"model": QWEN})
        await self.client.post("/rag/documents", json={"name": "a.txt", "content": "Paris is in France."})
        with patch.object(Embedder, "embed_query", autospec=True, side_effect=Embedder.embed_query) as q:
            res = await self.client.post("/rag/query", json={"query": "Where is Paris?"})
        self.assertEqual(res.status_code, 200, res.text)
        q.assert_called_once()
        self.assertEqual(res.json()["results"][0]["text"], "Paris is in France.")

    async def test_truncate_dim_flows_through_ingest_and_query(self):
        self.store.update_config({"model": QWEN, "truncate_dim": 256})
        r = await self.client.post("/rag/documents", json={"name": "a.txt", "content": "Paris is in France."})
        self.assertEqual(r.status_code, 201, r.text)
        res = await self.client.post("/rag/query", json={"query": "Paris"})
        self.assertEqual(res.status_code, 200, res.text)
        self.assertEqual(len(res.json()["results"]), 1)

    async def test_config_route_accepts_truncate_dim(self):
        r = await self.client.post("/rag/config", json={"model": QWEN, "truncate_dim": 512})
        self.assertEqual(r.status_code, 200, r.text)
        self.assertEqual(r.json()["truncate_dim"], 512)

    async def test_config_change_of_truncate_dim_with_documents_is_409(self):
        await self.client.post("/rag/documents", json={"name": "a.txt", "content": "hello world"})
        r = await self.client.post("/rag/config", json={"truncate_dim": 128})
        self.assertEqual(r.status_code, 409, r.text)


if __name__ == "__main__":
    unittest.main()
