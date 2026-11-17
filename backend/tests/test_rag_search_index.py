import os
import shutil
import tempfile
import unittest
from unittest.mock import patch

import numpy as np

from backend.rag import store as store_module
from backend.rag.store import RagStore

MODEL = "all-MiniLM-L6-v2"      # the default configured model, so add_document accepts it


def unit(rng, n, dim):
    v = rng.standard_normal((n, dim)).astype(np.float32)
    return v / np.linalg.norm(v, axis=1, keepdims=True)


class IndexTestCase(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.store = RagStore(self.dir)
        self.store.update_config({"model": MODEL})      # pin: the fresh-store default depends on the machine

    def fill(self, docs=10, per_doc=50, dim=64, seed=1):
        """Add random documents; returns (matrix of all vectors in insert order, doc id per row, chunk idx per row)."""
        rng = np.random.default_rng(seed)
        rows, doc_ids, chunk_idx = [], [], []
        for d in range(docs):
            vecs = unit(rng, per_doc, dim)
            doc = self.store.add_document(f"doc{d}", "txt", "", MODEL, [f"d{d} chunk {i}" for i in range(per_doc)],
                                          vecs, 1.0, 64, 8)
            rows.append(vecs)
            doc_ids += [doc.id] * per_doc
            chunk_idx += list(range(per_doc))
        return np.concatenate(rows), doc_ids, chunk_idx

    @staticmethod
    def oracle(matrix, doc_ids, chunk_idx, query, top_k, min_score=0.0, allowed=None):
        scores = matrix @ query
        out = []
        for i in np.argsort(-scores, kind="stable"):
            if allowed is not None and doc_ids[i] not in allowed:
                continue
            if min_score > 0 and scores[i] < min_score:
                break
            out.append((doc_ids[i], chunk_idx[i], round(float(scores[i]), 4)))
            if len(out) >= top_k:
                break
        return out

    @staticmethod
    def keys(results):
        return [(r.document_id, r.index, r.score) for r in results]

    def assert_same(self, got, want, ordered=True, msg=None):
        """Same chunks (and order if asked); scores equal to 4 decimals within float-rounding noise."""
        if not ordered:
            got, want = sorted(got), sorted(want)
        self.assertEqual([g[:2] for g in got], [w[:2] for w in want], msg)
        for g, w in zip(got, want):
            self.assertAlmostEqual(g[2], w[2], delta=1.5e-4, msg=msg)


class OracleTests(IndexTestCase):
    def queries(self, dim, n=12):
        return unit(np.random.default_rng(99), n, dim)

    def check_dtype(self, dtype, exact_order):
        with patch.dict(os.environ, {"MAGNITE_RAG_INDEX_DTYPE": dtype}):
            matrix, doc_ids, chunk_idx = self.fill()
            for q in self.queries(64):
                for top_k in (1, 4, 20):
                    for min_score in (0.0, 0.2):
                        got = self.keys(self.store.search(q, MODEL, top_k=top_k, min_score=min_score))
                        want = self.oracle(matrix, doc_ids, chunk_idx, q, top_k, min_score)
                        self.assert_same(got, want, ordered=exact_order, msg=(dtype, top_k, min_score))

    def test_float32_matches_oracle_exactly(self):
        self.check_dtype("float32", exact_order=True)

    def test_float16_returns_oracle_set_with_exact_scores(self):
        self.check_dtype("float16", exact_order=False)

    def test_int8_returns_oracle_set_with_exact_scores(self):
        self.check_dtype("int8", exact_order=False)

    def test_returned_order_is_descending_score(self):
        self.fill()
        res = self.store.search(self.queries(64, 1)[0], MODEL, top_k=20)
        scores = [r.score for r in res]
        self.assertEqual(scores, sorted(scores, reverse=True))

    def test_result_carries_text_name_and_index(self):
        self.fill(docs=2, per_doc=5)
        res = self.store.search(self.queries(64, 1)[0], MODEL, top_k=1)[0]
        self.assertEqual(res.text, f"d{res.document_name[-1]} chunk {res.index}")
        self.assertTrue(res.document_name.startswith("doc"))
        self.assertGreater(res.tokens, 0)

    def test_top_k_larger_than_corpus_returns_everything(self):
        self.fill(docs=2, per_doc=5)
        self.assertEqual(len(self.store.search(self.queries(64, 1)[0], MODEL, top_k=50)), 10)

    def test_empty_store_returns_empty(self):
        self.assertEqual(self.store.search(np.ones(8, dtype=np.float32), MODEL), [])


class FilterTests(IndexTestCase):
    def test_empty_document_ids_returns_nothing(self):
        self.fill()
        q = unit(np.random.default_rng(3), 1, 64)[0]
        self.assertEqual(self.store.search(q, MODEL, document_ids=[]), [])

    def test_unknown_document_id_returns_nothing(self):
        self.fill()
        q = unit(np.random.default_rng(3), 1, 64)[0]
        self.assertEqual(self.store.search(q, MODEL, document_ids=["nope"]), [])

    def test_document_ids_filter_matches_oracle(self):
        matrix, doc_ids, chunk_idx = self.fill()
        allowed = {doc_ids[0], doc_ids[120], doc_ids[300]}
        for q in unit(np.random.default_rng(5), 6, 64):
            got = self.keys(self.store.search(q, MODEL, top_k=7, document_ids=list(allowed)))
            self.assert_same(got, self.oracle(matrix, doc_ids, chunk_idx, q, 7, allowed=allowed), ordered=False)
            self.assertTrue(all(g[0] in allowed for g in got))

    def test_other_model_rows_are_ignored(self):
        matrix, doc_ids, chunk_idx = self.fill(docs=4, per_doc=10)
        other = doc_ids[0]
        with self.store._conn() as conn:
            conn.execute("UPDATE rag_documents SET model = 'some/other-model' WHERE id = ?", (other,))
            conn.commit()
        self.store._invalidate()
        q = unit(np.random.default_rng(8), 1, 64)[0]
        res = self.store.search(q, MODEL, top_k=100)
        self.assertEqual(len(res), 30)
        self.assertNotIn(other, {r.document_id for r in res})


class InvalidationTests(IndexTestCase):
    def test_index_is_built_once_while_warm_and_rebuilt_after_a_write(self):
        self.fill(docs=2, per_doc=5)
        q = unit(np.random.default_rng(2), 1, 64)[0]
        with patch.object(RagStore, "_build_index", autospec=True, side_effect=RagStore._build_index) as build:
            for _ in range(3):
                self.store.search(q, MODEL)
            self.assertEqual(build.call_count, 1)
            self.store.add_document("late", "txt", "", MODEL, ["x"], unit(np.random.default_rng(4), 1, 64), 1.0, 64, 8)
            self.store.search(q, MODEL)
            self.assertEqual(build.call_count, 2)

    def test_delete_invalidates_warm_cache(self):
        self.fill()
        q = unit(np.random.default_rng(6), 1, 64)[0]
        top = self.store.search(q, MODEL, top_k=3)[0]
        self.store.delete_document(top.document_id)
        after = self.store.search(q, MODEL, top_k=50)
        self.assertNotIn(top.document_id, {r.document_id for r in after})

    def test_add_document_invalidates_warm_cache(self):
        self.fill(docs=3, per_doc=10)
        q = unit(np.random.default_rng(7), 1, 64)[0]
        self.store.search(q, MODEL)                                   # warm the cache
        doc = self.store.add_document("planted", "txt", "", MODEL, ["planted chunk"], q.reshape(1, -1), 1.0, 64, 8)
        top = self.store.search(q, MODEL, top_k=1)[0]
        self.assertEqual(top.document_id, doc.id)
        self.assertAlmostEqual(top.score, 1.0, places=3)

    def test_clear_invalidates_warm_cache(self):
        self.fill()
        q = unit(np.random.default_rng(6), 1, 64)[0]
        self.assertTrue(self.store.search(q, MODEL))
        self.store.clear()
        self.assertEqual(self.store.search(q, MODEL), [])

    def test_reopened_store_searches_existing_data(self):
        matrix, doc_ids, chunk_idx = self.fill(docs=3, per_doc=10)
        reopened = RagStore(self.dir)
        q = unit(np.random.default_rng(10), 1, 64)[0]
        got = self.keys(reopened.search(q, MODEL, top_k=4))
        self.assert_same(got, self.oracle(matrix, doc_ids, chunk_idx, q, 4))


class DtypeAndBudgetTests(IndexTestCase):
    def build(self, env):
        with patch.dict(os.environ, env):
            self.fill(docs=2, per_doc=20)
            return self.store._index_for(MODEL)

    def test_default_dtype_is_float16_and_smaller_than_float32(self):
        index = self.build({"MAGNITE_RAG_INDEX_DTYPE": "float16"})
        self.assertEqual(index.matrix.dtype, np.float16)
        self.assertEqual(index.matrix.nbytes, 40 * 64 * 2)

    def test_unset_env_defaults_to_float16(self):
        env = {k: v for k, v in os.environ.items() if k != "MAGNITE_RAG_INDEX_DTYPE"}
        with patch.dict(os.environ, env, clear=True):
            self.fill(docs=1, per_doc=5)
            self.assertEqual(self.store._index_for(MODEL).matrix.dtype, np.float16)

    def test_int8_dtype(self):
        index = self.build({"MAGNITE_RAG_INDEX_DTYPE": "int8"})
        self.assertEqual(index.matrix.dtype, np.int8)
        self.assertEqual(index.matrix.nbytes, 40 * 64)

    def test_unknown_dtype_falls_back_to_float16(self):
        index = self.build({"MAGNITE_RAG_INDEX_DTYPE": "bogus"})
        self.assertEqual(index.matrix.dtype, np.float16)

    def test_over_budget_does_not_build_an_index_and_scans_in_batches(self):
        env = {"MAGNITE_RAG_INDEX_MB": "1", "MAGNITE_RAG_INDEX_DTYPE": "float32"}
        with patch.dict(os.environ, env), patch.object(store_module, "_SCAN_BATCH", 100):
            matrix, doc_ids, chunk_idx = self.fill(docs=6, per_doc=200, dim=256)     # 1200 x 1 KB > 1 MB
            self.assertIsNone(self.store._index_for(MODEL))
            for q in unit(np.random.default_rng(11), 5, 256):
                for allowed in (None, {doc_ids[0], doc_ids[700]}):
                    got = self.keys(self.store.search(q, MODEL, top_k=6, document_ids=None if allowed is None else list(allowed)))
                    self.assert_same(got, self.oracle(matrix, doc_ids, chunk_idx, q, 6, allowed=allowed))


class HydrationTests(IndexTestCase):
    def test_only_top_candidates_are_hydrated(self):
        self.fill(docs=10, per_doc=50)
        q = unit(np.random.default_rng(12), 1, 64)[0]
        seen = []
        original = RagStore._hydrate

        def spy(self_, ids):
            seen.append(len(ids))
            return original(self_, ids)

        with patch.object(RagStore, "_hydrate", spy):
            self.store.search(q, MODEL, top_k=4)
        self.assertEqual(len(seen), 1)
        self.assertLessEqual(seen[0], max(2 * 4, 4 + 16))
        self.assertGreaterEqual(seen[0], 4)

    def test_query_dimension_mismatch_raises_clear_error(self):
        self.fill(docs=2, per_doc=5)
        with self.assertRaises(ValueError) as ctx:
            self.store.search(np.ones(3, dtype=np.float32), MODEL)
        self.assertIn("dimension", str(ctx.exception))


if __name__ == "__main__":
    unittest.main()
