import os
import shutil
import sqlite3
import tempfile
import unittest
from unittest.mock import patch

import httpx
import numpy as np

from backend.rag import embeddings, keyword, memguard
from backend.rag.schemas import RagConfig
from backend.rag.store import RagStore
from backend.routes import rag
from backend.server import app

MODEL = "all-MiniLM-L6-v2"
Q = np.array([1.0, 0.0, 0.0], dtype=np.float32)


def unit(*values):
    v = np.array(values, dtype=np.float32)
    return v / np.linalg.norm(v)


class HybridStoreCase(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.store = RagStore(self.dir)
        self.store.update_config({"model": MODEL})

    def add(self, name, rows):
        """rows: (text, vector) pairs."""
        return self.store.add_document(name, "txt", "", MODEL, [t for t, _ in rows],
                                       np.stack([v for _, v in rows]), 1.0, 64, 8)

    def many_fillers(self, n=40):
        """Chunks all closer to the query than any keyword-only chunk, so they fill the vector candidates."""
        return [(f"filler number {i} about nothing", unit(1.0, 0.01 * (i + 1), 0.0)) for i in range(n)]

    def search(self, text, top_k=4, mode="hybrid", **kw):
        return self.store.search(Q, MODEL, top_k=top_k, query_text=text, mode=mode, **kw)


class HybridTests(HybridStoreCase):
    def build_id_library(self):
        self.add("main", self.many_fillers() + [("the invoice INV-77341 is overdue", unit(0.0, 0.0, 1.0))])

    def test_hybrid_finds_exact_id(self):
        self.build_id_library()
        vector = self.search("INV-77341", top_k=2, mode="vector")
        self.assertFalse(any("INV-77341" in r.text for r in vector))
        hybrid = self.search("INV-77341", top_k=2)
        hit = [r for r in hybrid if "INV-77341" in r.text]
        self.assertEqual(len(hit), 1)
        self.assertEqual(hit[0].match, "keyword")
        self.assertAlmostEqual(hit[0].score, 0.0, places=4)      # exact cosine, not an RRF value
        self.assertEqual(hit[0].fused_score, round(1 / 61, 6))

    def test_vector_mode_default_unchanged(self):
        self.build_id_library()
        plain = self.store.search(Q, MODEL, top_k=3)
        vector = self.search("INV-77341", top_k=3, mode="vector")
        self.assertEqual([(r.document_id, r.index, r.score) for r in plain],
                         [(r.document_id, r.index, r.score) for r in vector])
        for r in plain + vector:
            self.assertEqual((r.match, r.fused_score), ("vector", None))

    def test_both_legs_marked_and_fused_order(self):
        self.add("d", [("alpha plain", unit(1.0, 0.0, 0.0)),
                       ("gizmo manual", unit(1.0, 0.2, 0.0)),
                       ("unrelated", unit(0.0, 1.0, 0.0))])
        res = self.search("gizmo", top_k=3)
        self.assertEqual([r.text for r in res][:2], ["gizmo manual", "alpha plain"])
        self.assertEqual([r.match for r in res][:2], ["both", "vector"])
        self.assertTrue(res[0].fused_score > res[1].fused_score)

    def test_score_is_cosine_and_fused_score_rrf(self):
        self.add("d", [("gizmo manual", unit(1.0, 0.2, 0.0)), ("other words", unit(0.5, 1.0, 0.0))])
        top = self.search("gizmo", top_k=2)[0]
        self.assertEqual(top.fused_score, round(1 / 61 + 1 / 61, 6))
        self.assertAlmostEqual(top.score, float(unit(1.0, 0.2, 0.0) @ Q), places=4)

    def test_ties_go_to_higher_cosine_then_lower_id(self):
        self.build_id_library()
        res = self.search("invoice", top_k=2)             # a plain word: no identifier in the query
        self.assertEqual(res[0].match, "vector")          # rank 1 in one leg each: tie, higher cosine first
        self.assertEqual(res[1].match, "keyword")

    def test_a_chunk_holding_the_queried_identifier_verbatim_outranks_the_rest(self):
        self.build_id_library()
        res = self.search("INV-77341", top_k=2)
        self.assertEqual(res[0].match, "keyword")         # the identifier chunk, ahead of the closer-by-meaning filler
        self.assertIn("INV-77341", res[0].text)
        self.assertEqual(res[0].fused_score, round(1 / 61, 6))      # the reported value is still the two-leg RRF

    def test_min_score_filters_keyword_hits_by_cosine(self):
        self.build_id_library()
        self.assertTrue(any("INV-77341" in r.text for r in self.search("INV-77341", top_k=2)))
        res = self.search("INV-77341", top_k=2, min_score=0.3)
        self.assertEqual(len(res), 2)
        self.assertFalse(any("INV-77341" in r.text for r in res))
        self.assertTrue(all(r.score >= 0.3 for r in res))

    def test_document_ids_and_model_filter_apply_to_keyword_leg(self):
        a = self.add("a", [("shared SHARED-9 in a", unit(0.0, 1.0, 0.0))])
        b = self.add("b", [("shared SHARED-9 in b", unit(0.0, 0.0, 1.0))])
        both = {r.document_id for r in self.search("SHARED-9", top_k=4)}
        self.assertEqual(both, {a.id, b.id})
        only_b = self.search("SHARED-9", top_k=4, document_ids=[b.id])
        self.assertEqual({r.document_id for r in only_b}, {b.id})
        self.assertEqual(self.search("SHARED-9", document_ids=[]), [])
        conn = sqlite3.connect(os.path.join(self.dir, "rag.db"))
        conn.execute("UPDATE rag_documents SET model = 'stale-model' WHERE id = ?", (a.id,))
        conn.commit()
        conn.close()
        self.store._invalidate()                        # the out-of-band edit bypassed the store's cache
        self.assertEqual({r.document_id for r in self.search("SHARED-9", top_k=4)}, {b.id})

    def test_keyword_error_falls_back(self):
        self.build_id_library()
        with patch.object(RagStore, "_keyword_ids", side_effect=sqlite3.OperationalError("boom")):
            res = self.search("INV-77341", top_k=2)
        self.assertEqual([r.match for r in res], ["vector", "vector"])
        self.assertEqual([r.fused_score for r in res], [None, None])

    def test_unsearchable_text_is_vector_only(self):
        self.build_id_library()
        for text in ('"', "NEAR(", "a:b", "---", "", "   "):
            res = self.search(text, top_k=2)
            self.assertEqual([r.match for r in res], ["vector", "vector"], text)

    def test_fts5_missing_is_vector_only(self):
        shutil.rmtree(self.dir)
        os.makedirs(self.dir)
        with patch.object(keyword, "fts5_available", return_value=False):
            store = RagStore(self.dir)
            store.update_config({"model": MODEL})
            store.add_document("d", "txt", "", MODEL, ["a ZZ-1", "b"], np.stack([unit(1, 0, 0), unit(0, 1, 0)]),
                               1.0, 64, 8)
            res = store.search(Q, MODEL, top_k=2, query_text="ZZ-1", mode="hybrid")
        self.assertEqual([r.match for r in res], ["vector", "vector"])

    def test_nasty_text_never_raises(self):
        self.build_id_library()
        for text in ['"', 'NEAR(a b)', "col:val", "ERR-404", "a*", "^start", "(x", "AND", "OR OR", "it's", "日本語"]:
            self.search(text, top_k=2)


class ConfigSchemaTests(HybridStoreCase):
    def test_default_and_unknown_stored_value(self):
        self.assertEqual(RagConfig().search_mode, "hybrid")
        conn = sqlite3.connect(os.path.join(self.dir, "rag.db"))
        conn.execute("INSERT OR REPLACE INTO rag_config (key, value) VALUES ('search_mode', 'bogus')")
        conn.commit()
        conn.close()
        self.assertEqual(self.store.get_config().search_mode, "hybrid")

    def test_update_roundtrip(self):
        self.assertEqual(self.store.update_config({"search_mode": "vector"}).search_mode, "vector")
        self.assertEqual(RagStore(self.dir).get_config().search_mode, "vector")


class RouteTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        env = patch.dict(os.environ, {"MAGNITE_HOME": self.temp.name})
        env.start()
        self.addCleanup(env.stop)
        rag._rag_setup()
        self.store = rag._get_store()

        def vector_for(text):
            rng = np.random.default_rng(abs(hash(text)) % (2 ** 32))
            v = rng.standard_normal(384).astype(np.float32)
            return v / np.linalg.norm(v)

        for p in (patch.object(embeddings, "is_available", return_value=True),
                  patch.object(embeddings, "embed_documents",
                               side_effect=lambda texts, model, d=0: np.stack([vector_for(t) for t in texts])),
                  patch.object(embeddings, "embed_query", side_effect=lambda t, m, d=0: vector_for(t)),
                  patch.object(memguard, "available_memory_mb", return_value=50_000.0)):
            p.start()
            self.addCleanup(p.stop)
        embeddings._reset_for_tests()
        self.addCleanup(embeddings._reset_for_tests)
        self.client = httpx.AsyncClient(transport=httpx.ASGITransport(app=app, raise_app_exceptions=False),
                                        base_url="http://test")
        self.addAsyncCleanup(self.client.aclose)
        await self.client.post("/rag/config", json={"model": embeddings.MINILM})
        for name, content in (("a.txt", "Ordinary notes about gardens."), ("b.txt", "The code is INV-77341 exactly.")):
            r = await self.client.post("/rag/documents", json={"name": name, "content": content})
            self.assertEqual(r.status_code, 201, r.text)

    async def query(self, text, **extra):
        return await self.client.post("/rag/query", json={"query": text, "top_k": 4, **extra})

    async def test_route_nasty_queries_200(self):
        for text in ['"', "NEAR(", "a:b", "---", "AND OR", "(x", "a*"]:
            r = await self.query(text)
            self.assertEqual(r.status_code, 200, (text, r.text))
            self.assertTrue(r.json()["results"], text)

    async def test_status_reports_search(self):
        search = (await self.client.get("/rag/status")).json()["search"]
        self.assertEqual(search, {"mode": "hybrid", "requested_mode": "hybrid", "keyword_available": True, "rrf_k": 60})
        await self.client.post("/rag/config", json={"search_mode": "vector"})
        search = (await self.client.get("/rag/status")).json()["search"]
        self.assertEqual((search["mode"], search["requested_mode"], search["keyword_available"]),
                         ("vector", "vector", True))
        await self.client.post("/rag/config", json={"search_mode": "hybrid"})
        with patch.object(RagStore, "keyword_available", new=False):
            search = (await self.client.get("/rag/status")).json()["search"]
        self.assertEqual((search["mode"], search["requested_mode"], search["keyword_available"]),
                         ("vector", "hybrid", False))

    async def test_config_search_mode_roundtrip_and_query_override(self):
        cfg = (await self.client.post("/rag/config", json={"search_mode": "vector"})).json()
        self.assertEqual(cfg["search_mode"], "vector")
        bad = await self.client.post("/rag/config", json={"search_mode": "fuzzy"})
        self.assertEqual(bad.status_code, 422)
        results = (await self.query("INV-77341")).json()["results"]
        self.assertTrue(results)
        self.assertEqual({(r["match"], r["fused_score"]) for r in results}, {("vector", None)})
        results = (await self.query("INV-77341", search_mode="hybrid")).json()["results"]
        hit = [r for r in results if "INV-77341" in r["text"]]
        self.assertEqual(len(hit), 1)
        self.assertEqual(hit[0]["match"], "both")
        self.assertIsNotNone(hit[0]["fused_score"])
        await self.client.post("/rag/config", json={"search_mode": "hybrid"})
        results = (await self.query("INV-77341", search_mode="vector")).json()["results"]
        self.assertEqual({r["match"] for r in results}, {"vector"})
        self.assertEqual((await self.query("x", search_mode="fuzzy")).status_code, 422)


if __name__ == "__main__":
    unittest.main()
