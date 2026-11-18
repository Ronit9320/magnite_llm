"""Bounds for hybrid search on a large library. Slow: set MAGNITE_SLOW_TESTS=1 to run it."""
import os
import shutil
import sqlite3
import statistics
import tempfile
import time
import tracemalloc
import unittest

import numpy as np

from backend.rag.store import RagStore

MODEL = "all-MiniLM-L6-v2"
CHUNKS = 50_000
BATCH = 1_000
DIM = 384
WORDS_PER_CHUNK = 150
TARGET = 31_337


def vocabulary(size=3000):
    """Zipf-like word list: the first words ('the', 'system', 'data') occur in nearly every chunk."""
    words = ["the", "system", "data"] + [f"word{i}" for i in range(size - 3)]
    weights = 1.0 / np.arange(1, size + 1)
    return words, weights / weights.sum()


def median_ms(fn, runs=5):
    times = []
    for _ in range(runs):
        started = time.perf_counter()
        fn()
        times.append((time.perf_counter() - started) * 1000)
    return statistics.median(times)


@unittest.skipUnless(os.environ.get("MAGNITE_SLOW_TESTS") == "1", "slow: set MAGNITE_SLOW_TESTS=1")
class HybridScaleTests(unittest.TestCase):
    def test_50k_chunks(self):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp, ignore_errors=True)
        store = RagStore(tmp)
        store.update_config({"model": MODEL})
        rng = np.random.default_rng(7)
        words, weights = vocabulary()
        words = np.array(words)
        for start in range(0, CHUNKS, BATCH):
            picks = rng.choice(len(words), size=(BATCH, WORDS_PER_CHUNK), p=weights)
            texts = [" ".join(words[row]) + f" ID-{start + i}" for i, row in enumerate(picks)]
            vecs = rng.standard_normal((BATCH, DIM)).astype(np.float32)
            vecs /= np.linalg.norm(vecs, axis=1, keepdims=True)
            store.add_document(f"doc{start // BATCH}", "txt", "", MODEL, texts, vecs, 1.0, 320, 40)
        self.assertEqual(store.count_chunks(), CHUNKS)
        db = os.path.join(tmp, "rag.db")

        conn = sqlite3.connect(db)                          # force a rebuild, as after an out-of-band change
        conn.execute("DELETE FROM rag_fts_state")
        conn.commit()
        conn.close()
        store._fts_synced = False
        started = time.perf_counter()
        self.assertTrue(store._ensure_keyword_index())
        rebuild_s = time.perf_counter() - started
        self.assertLess(rebuild_s, 10)

        size_with = os.path.getsize(db)
        copy = os.path.join(tmp, "no_fts.db")
        shutil.copyfile(db, copy)
        conn = sqlite3.connect(copy)
        conn.execute("DROP TABLE rag_chunks_fts")
        conn.execute("DROP TABLE rag_fts_state")
        conn.commit()
        conn.execute("VACUUM")
        conn.close()
        size_without = os.path.getsize(copy)

        id_ms = median_ms(lambda: store._keyword_ids('"ID-31337"', MODEL, None, 17))
        common_ms = median_ms(lambda: store._keyword_ids('"the" OR "system" OR "data"', MODEL, None, 17))
        self.assertLess(id_ms, 250)
        self.assertLess(common_ms, 250)

        query = rng.standard_normal(DIM).astype(np.float32)
        query /= np.linalg.norm(query)
        found = store.search(query, MODEL, top_k=4, query_text=f"ID-{TARGET}", mode="hybrid")      # warm-up: builds the vector index
        self.assertIn(f"ID-{TARGET}", [w for r in found for w in r.text.split()])
        hit = next(r for r in found if r.text.endswith(f"ID-{TARGET}"))
        self.assertEqual(hit.match, "keyword")             # random vectors: found by the keyword leg only
        self.assertLessEqual(found.index(hit), 1)          # a keyword-only rank 1 ties vector rank 1; cosine breaks it

        tracemalloc.start()
        try:
            for _ in range(5):
                store.search(query, MODEL, top_k=4, query_text=f"ID-{TARGET}", mode="hybrid")
            peak_mb = tracemalloc.get_traced_memory()[1] / (1024 * 1024)
        finally:
            tracemalloc.stop()
        self.assertLess(peak_mb, 50)

        print(f"\n[scale] chunks={CHUNKS} rebuild={rebuild_s:.2f}s id_query={id_ms:.1f}ms common_query={common_ms:.1f}ms "
              f"py_heap_peak={peak_mb:.1f}MB rag.db with_fts={size_with / 1e6:.1f}MB without_fts={size_without / 1e6:.1f}MB "
              f"fts_overhead={(size_with - size_without) / 1e6:.1f}MB")


if __name__ == "__main__":
    unittest.main()
