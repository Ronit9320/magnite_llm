"""The in-memory search index follows adds and deletes instead of being rebuilt (break report 2026-11-20, F6).

Before: every write dropped the index, so the first query after any ingest or delete re-read every
vector from SQLite (4.6 s at 150k chunks) while holding the store lock.
"""
import os
import random
import shutil
import tempfile
import unittest
from unittest.mock import patch

import numpy as np

from backend.rag.store import RagStore

MODEL = "all-MiniLM-L6-v2"
DIM = 16


def unit_rows(rng, n):
    m = rng.standard_normal((n, DIM)).astype(np.float32)
    return m / np.linalg.norm(m, axis=1, keepdims=True)


class IncrementalIndexTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.store = RagStore(self.dir)
        self.store.update_config({"model": MODEL})
        self.rng = np.random.default_rng(3)
        self.n = 0

    def add(self, per=8, name=None):
        self.n += 1
        return self.store.add_document(name or f"d{self.n}", "txt", "", MODEL, [f"doc{self.n} chunk{i} text" for i in range(per)],
                                       unit_rows(self.rng, per), 1.0, 64, 8)

    def keys(self, store, q, **kw):
        return [(r.document_id, r.index, r.score) for r in store.search(q, MODEL, top_k=kw.pop("top_k", 6), mode="vector", **kw)]

    def fresh(self):
        return RagStore(self.dir)                  # a new instance builds its own index from SQLite

    def builds(self):
        return patch.object(RagStore, "_build_index", autospec=True, side_effect=RagStore._build_index)

    def test_adding_a_document_does_not_rebuild_the_warm_index(self):
        self.add(); self.add()
        q = unit_rows(self.rng, 1)[0]
        with self.builds() as build:
            self.store.search(q, MODEL)
            self.add(per=5)
            self.store.search(q, MODEL)
            self.assertEqual(build.call_count, 1)

    def test_deleting_a_document_does_not_rebuild_the_warm_index(self):
        a, _ = self.add(), self.add()
        q = unit_rows(self.rng, 1)[0]
        with self.builds() as build:
            self.store.search(q, MODEL)
            self.store.delete_document(a.id)
            found = self.store.search(q, MODEL, top_k=50)
            self.assertEqual(build.call_count, 1)
        self.assertNotIn(a.id, {r.document_id for r in found})

    def test_a_new_document_is_found_exactly_and_by_filter(self):
        self.add(); self.add()
        q = unit_rows(self.rng, 1)[0]
        self.store.search(q, MODEL)                                        # warm
        planted = self.store.add_document("planted", "txt", "", MODEL, ["planted"], q.reshape(1, -1), 1.0, 64, 8)
        top = self.store.search(q, MODEL, top_k=1, mode="vector")[0]
        self.assertEqual((top.document_id, round(top.score, 3)), (planted.id, 1.0))
        only = self.store.search(q, MODEL, top_k=5, mode="vector", document_ids=[planted.id])
        self.assertEqual([r.document_id for r in only], [planted.id])

    def test_the_first_document_into_an_empty_warm_index(self):
        q = unit_rows(self.rng, 1)[0]
        self.assertEqual(self.store.search(q, MODEL), [])                  # warms an empty index
        doc = self.add(per=3)
        self.assertEqual({r.document_id for r in self.store.search(q, MODEL, top_k=5)}, {doc.id})

    def test_deleting_the_last_document_leaves_an_empty_index(self):
        doc = self.add()
        q = unit_rows(self.rng, 1)[0]
        self.store.search(q, MODEL)
        self.store.delete_document(doc.id)
        self.assertEqual(self.store.search(q, MODEL), [])
        self.add(per=2)
        self.assertEqual(len(self.store.search(q, MODEL, top_k=5)), 2)

    def test_random_adds_and_deletes_always_match_a_freshly_built_index(self):
        for dtype in ("float16", "float32", "int8"):
            with self.subTest(dtype=dtype), patch.dict(os.environ, {"MAGNITE_RAG_INDEX_DTYPE": dtype}):
                self.store.clear()
                rng, live = random.Random(11), []
                q = unit_rows(self.rng, 1)[0]
                self.store.search(q, MODEL)                                # keep the index warm throughout
                for step in range(60):
                    if rng.random() < 0.6 or not live:
                        live.append(self.add(per=rng.randint(1, 12)).id)
                    else:
                        self.store.delete_document(live.pop(rng.randrange(len(live))))
                    if step % 6 == 0:
                        probe = unit_rows(self.rng, 1)[0]
                        subset = rng.sample(live, min(len(live), 3)) if live else []
                        self.assertEqual(self.keys(self.store, probe), self.keys(self.fresh(), probe), f"step {step}")
                        self.assertEqual(self.keys(self.store, probe, document_ids=subset),
                                         self.keys(self.fresh(), probe, document_ids=subset), f"step {step} filtered")

    def test_growing_past_the_memory_budget_falls_back_to_the_scan_and_stays_exact(self):
        self.add(per=10)
        q = unit_rows(self.rng, 1)[0]
        with patch.dict(os.environ, {"MAGNITE_RAG_INDEX_MB": str(10 * DIM * 2 * 1.5 / 1024 / 1024)}):
            self.store._invalidate()
            self.store.search(q, MODEL)                                    # fits: warm
            self.add(per=10)                                               # now over budget
            self.assertNotIn(MODEL, self.store._indexes)
            self.assertEqual(self.keys(self.store, q), self.keys(self.fresh(), q))

    def test_clear_still_drops_everything(self):
        self.add()
        q = unit_rows(self.rng, 1)[0]
        self.store.search(q, MODEL)
        self.store.clear()
        self.assertEqual(self.store.search(q, MODEL), [])

    def test_keyword_search_stays_in_sync_after_incremental_writes(self):
        self.add()
        q = unit_rows(self.rng, 1)[0]
        self.store.search(q, MODEL, query_text="doc1", mode="hybrid")
        doc = self.store.add_document("k", "txt", "", MODEL, ["a zanzibar keyword chunk"], unit_rows(self.rng, 1), 1.0, 64, 8)
        found = self.store.search(q, MODEL, top_k=3, query_text="zanzibar", mode="hybrid")
        self.assertIn(doc.id, [r.document_id for r in found])
        self.store.delete_document(doc.id)
        self.assertNotIn(doc.id, [r.document_id for r in self.store.search(q, MODEL, top_k=3, query_text="zanzibar", mode="hybrid")])


if __name__ == "__main__":
    unittest.main()
