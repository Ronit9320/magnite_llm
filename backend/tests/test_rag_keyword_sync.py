import os
import shutil
import sqlite3
import tempfile
import unittest
from unittest.mock import patch

import numpy as np

from backend.rag import keyword
from backend.rag import store as store_module
from backend.rag.store import RagStore

MODEL = "all-MiniLM-L6-v2"
OTHER_MODEL = "other-fake-model"


def vec(*values):
    v = np.array(values, dtype=np.float32)
    return v / np.linalg.norm(v)


class KeywordSyncCase(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.store = RagStore(self.dir)
        self.store.update_config({"model": MODEL})

    def add(self, name, texts, store=None):
        store = store or self.store
        embeddings = np.stack([vec(1, i + 1, 2) for i in range(len(texts))])
        return store.add_document(name, "txt", "", MODEL, texts, embeddings, 1.0, 64, 8)

    def find(self, term, store=None, model=MODEL, allowed=None):
        store = store or self.store
        store._ensure_keyword_index()
        return store._keyword_ids(f'"{term}"', model, allowed, 10)

    def raw(self):
        conn = sqlite3.connect(os.path.join(self.dir, "rag.db"))
        self.addCleanup(conn.close)
        return conn

    def tables(self):
        return {r[0] for r in self.raw().execute("SELECT name FROM sqlite_master")}

    def raw_add_chunk(self, doc_id, text, new_doc=True):
        conn = self.raw()
        if new_doc:
            conn.execute("INSERT INTO rag_documents (id, name, kind, model, chunks) VALUES (?, 'raw', 'txt', ?, 1)",
                         (doc_id, MODEL))
        blob = vec(1, 1, 1).tobytes()
        conn.execute("INSERT INTO rag_chunks (doc_id, chunk_index, text, tokens, embedding) VALUES (?, 0, ?, 1, ?)",
                     (doc_id, text, blob))
        conn.commit()


class SyncTests(KeywordSyncCase):
    def test_ingest_indexes_text(self):
        doc = self.add("a", ["plain words", "serial ZX-9000 here"])
        ids = self.find("ZX-9000")
        self.assertEqual(len(ids), 1)
        row = self.store._hydrate(ids)[0]
        self.assertEqual((row["doc_id"], row["chunk_index"]), (doc.id, 1))

    def test_delete_and_clear_remove_rows(self):
        doc = self.add("a", ["serial ZX-9000 here"])
        self.add("b", ["another QQ-1 chunk"])
        self.assertTrue(self.store.delete_document(doc.id))
        self.assertEqual(self.find("ZX-9000"), [])
        self.assertEqual(len(self.find("QQ-1")), 1)
        self.store.clear()
        self.assertEqual(self.find("QQ-1"), [])
        self.add("c", ["fresh ZX-9000 again"])
        self.assertEqual(len(self.find("ZX-9000")), 1)

    def test_pre_existing_library_rebuilt(self):
        shutil.rmtree(self.dir)
        os.makedirs(self.dir)
        conn = sqlite3.connect(os.path.join(self.dir, "rag.db"))
        conn.execute(store_module._DOC_SCHEMA)
        conn.executescript(store_module._CHUNK_SCHEMA)
        conn.execute(store_module._CONFIG_SCHEMA)
        conn.commit()
        conn.close()
        self.raw_add_chunk("olddoc", "legacy token OLD-77")
        store = RagStore(self.dir)
        self.assertEqual(len(self.find("OLD-77", store=store)), 1)

    def test_out_of_band_writes_detected(self):
        self.add("a", ["keep KEEP-1"])
        doomed = self.add("b", ["doomed DOOM-2"])
        self.assertEqual(len(self.find("DOOM-2")), 1)
        conn = self.raw()                                     # an older build: no FTS bookkeeping
        conn.execute("DELETE FROM rag_chunks WHERE doc_id = ?", (doomed.id,))
        conn.execute("DELETE FROM rag_documents WHERE id = ?", (doomed.id,))
        conn.commit()
        self.raw_add_chunk("oob", "outofband NEW-3")
        fresh = RagStore(self.dir)
        self.assertEqual(len(self.find("NEW-3", store=fresh)), 1)
        self.assertEqual(self.find("DOOM-2", store=fresh), [])
        self.assertEqual(len(self.find("KEEP-1", store=fresh)), 1)

    def test_write_after_out_of_band_change_still_rebuilds(self):
        self.add("a", ["alpha ALPHA-1"])
        other = self.add("b", ["beta BETA-2"])
        self.raw_add_chunk("oob", "outofband OOB-9")
        fresh = RagStore(self.dir)
        self.assertTrue(fresh.delete_document(other.id))      # a write before any search
        self.assertEqual(len(self.find("OOB-9", store=fresh)), 1)
        self.assertEqual(self.find("BETA-2", store=fresh), [])
        self.assertEqual(len(self.find("ALPHA-1", store=fresh)), 1)

    def test_write_after_out_of_band_ingest_rebuilds(self):
        self.add("a", ["alpha ALPHA-1"])
        self.raw_add_chunk("oob", "outofband OOB-9")
        fresh = RagStore(self.dir)
        self.add("c", ["gamma GAMMA-3"], store=fresh)
        for term in ("OOB-9", "ALPHA-1", "GAMMA-3"):
            self.assertEqual(len(self.find(term, store=fresh)), 1, term)

    def test_reindex_keeps_keyword_index(self):
        self.add("a", ["serial ZX-9000 here"])
        self.find("ZX-9000")
        state_before = self.raw().execute("SELECT key, value FROM rag_fts_state ORDER BY key").fetchall()

        def embed(texts):
            return np.stack([vec(2, 1, i + 1) for i in range(len(texts))])

        self.store.reindex(OTHER_MODEL, 0, embed)
        self.assertEqual(len(self.find("ZX-9000", model=OTHER_MODEL)), 1)
        self.assertEqual(self.raw().execute("SELECT key, value FROM rag_fts_state ORDER BY key").fetchall(),
                         state_before)

    def test_fts_error_forces_rebuild(self):
        self.add("a", ["serial ZX-9000 here"])
        self.assertEqual(len(self.find("ZX-9000")), 1)
        conn = self.raw()
        conn.execute("DROP TABLE rag_chunks_fts")              # next MATCH raises "no such table"
        with self.assertRaises(sqlite3.Error):
            self.store._keyword_ids('"ZX-9000"', MODEL, None, 10)
        self.assertEqual(self.raw().execute("SELECT COUNT(*) FROM rag_fts_state").fetchone()[0], 0)
        self.assertEqual(len(self.find("ZX-9000")), 1)         # next use rebuilds
        self.assertTrue(self.store.keyword_available)

    def test_failed_fts_delete_is_not_hidden_by_a_fresh_state(self):
        keep = self.add("keep", ["serial ZX-9000 here"])
        gone = self.add("gone", ["another QQ-1 chunk"])
        self.store._ensure_keyword_index()
        self.raw().execute(f"DROP TABLE {keyword.FTS_TABLE}")      # the 'delete' command will fail
        self.assertTrue(self.store.delete_document(gone.id))
        self.assertEqual(len(self.find("ZX-9000")), 1)                # recreated table must be rebuilt
        self.assertTrue(self.store.get_document(keep.id))

    def test_failed_fts_clear_is_not_hidden_by_a_fresh_state(self):
        self.add("a", ["serial ZX-9000 here"])
        self.store._ensure_keyword_index()
        self.raw().execute(f"DROP TABLE {keyword.FTS_TABLE}")
        self.store.clear()
        self.add("b", ["fresh QQ-1 chunk"])
        self.assertEqual(len(self.find("QQ-1")), 1)

    def test_allowed_documents_filter(self):
        a = self.add("a", ["shared SHARED-1"])
        b = self.add("b", ["shared SHARED-1"])
        self.assertEqual(len(self.find("SHARED-1")), 2)
        only_a = self.find("SHARED-1", allowed={a.id})
        self.assertEqual([self.store._hydrate([i])[0]["doc_id"] for i in only_a], [a.id])
        self.assertEqual(self.find("SHARED-1", allowed=set()), [])
        many = {f"d{i}" for i in range(2000)} | {b.id}
        self.assertEqual(len(self.find("SHARED-1", allowed=many)), 1)

    def test_model_filter(self):
        self.add("a", ["serial ZX-9000 here"])
        self.assertEqual(self.find("ZX-9000", model="some-other-model"), [])

    def test_failed_ingest_leaves_index_consistent(self):
        self.add("a", ["alpha ALPHA-1"])
        with self.assertRaises(ValueError):
            self.store.add_document("bad", "txt", "", "wrong-model", ["x BAD-1"], np.stack([vec(1, 2, 3)]), 1.0, 64, 8)
        self.assertEqual(self.find("BAD-1"), [])
        self.assertEqual(len(self.find("ALPHA-1")), 1)


class NoFts5Tests(KeywordSyncCase):
    def test_no_fts5_runtime(self):
        shutil.rmtree(self.dir)
        os.makedirs(self.dir)
        with patch.object(keyword, "fts5_available", return_value=False):
            store = RagStore(self.dir)
            store.update_config({"model": MODEL})
            self.assertFalse(store.keyword_available)
            doc = self.add("a", ["serial ZX-9000 here"], store=store)
            self.add("b", ["other chunk"], store=store)
            self.assertFalse(store._ensure_keyword_index())
            self.assertTrue(store.delete_document(doc.id))
            store.clear()
            self.assertEqual(store.count_chunks(), 0)
        self.assertNotIn("rag_chunks_fts", self.tables())

    def test_missing_module_with_existing_table_still_writes(self):
        self.add("a", ["serial ZX-9000 here"])
        with patch.object(keyword, "fts5_available", return_value=False):
            store = RagStore(self.dir)
            self.assertFalse(store.keyword_available)
            self.add("b", ["more MORE-1"], store=store)
            store.clear()
        fresh = RagStore(self.dir)                              # FTS is back: stale index is rebuilt
        self.add("c", ["third THIRD-3"], store=fresh)
        self.assertEqual(self.find("ZX-9000", store=fresh), [])
        self.assertEqual(len(self.find("THIRD-3", store=fresh)), 1)


class RebuildFailureTests(KeywordSyncCase):
    def test_failed_rebuild_turns_keyword_search_off(self):
        self.add("a", ["serial ZX-9000 here"])
        conn = self.raw()
        conn.execute("DELETE FROM rag_fts_state")
        conn.commit()
        self.store._fts_synced = False

        def boom(self, conn):
            raise sqlite3.OperationalError("rebuild failed")

        with patch.object(store_module.RagStore, "_rebuild_keyword_index", boom):
            self.assertFalse(self.store._ensure_keyword_index())
        self.assertFalse(self.store.keyword_available)
        self.add("b", ["writes still work"])                    # vector-only from here on
        self.assertEqual(self.store.count_chunks(), 2)


if __name__ == "__main__":
    unittest.main()
