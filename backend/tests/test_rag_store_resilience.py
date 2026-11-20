"""The store keeps working around damage and reports a busy database clearly.

Break report 2026-11-20: F13 (one truncated embedding row blocked all search and ingest) and
F5 (a database locked by another program turned every endpoint into a bare 500).
"""
import asyncio
import os
import shutil
import sqlite3
import tempfile
import threading
import unittest
from unittest.mock import patch

import numpy as np
from fastapi import HTTPException

from backend.rag.store import RagStore, StoreBusy
from backend.routes import rag

from test_rag_large_ingest import IngestTestCase

MODEL = "all-MiniLM-L6-v2"
DIM = 4


def unit(*values):
    v = np.array(values, dtype=np.float32)
    return v / np.linalg.norm(v)


class DamagedRowTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.store = RagStore(self.dir)
        self.store.update_config({"model": MODEL})
        self.a = self.add("a", ["alpha one gizmo", "alpha two"], [unit(1, 0, 0, 0), unit(0.9, 0.1, 0, 0)])
        self.b = self.add("b", ["bravo one", "bravo two"], [unit(0, 1, 0, 0), unit(0.1, 0.9, 0, 0)])
        self.q = unit(0.2, 1, 0, 0)

    def add(self, name, texts, vecs):
        return self.store.add_document(name, "txt", "", MODEL, texts, np.stack(vecs), 1.0, 64, 8)

    def damage(self, doc, index, blob=b"\x01\x02\x03"):
        with self.store._conn() as conn:
            conn.execute("UPDATE rag_chunks SET embedding = ? WHERE doc_id = ? AND chunk_index = ?", (blob, doc.id, index))
        self.store._invalidate()

    def search(self, mode="hybrid", text="bravo", **kw):
        return self.store.search(self.q, MODEL, top_k=4, query_text=text, mode=mode, **kw)

    def test_search_skips_a_damaged_row_and_serves_the_rest(self):
        self.damage(self.a, 0)
        for mode in ("vector", "hybrid"):
            with self.subTest(mode):
                found = self.search(mode)
                self.assertTrue(any("bravo" in r.text for r in found))
                self.assertNotIn("alpha one gizmo", [r.text for r in found])

    def test_a_damaged_row_found_by_keyword_is_dropped_not_fatal(self):
        self.damage(self.a, 0)
        found = self.search("hybrid", text="gizmo")                  # the keyword leg finds the damaged chunk
        self.assertNotIn("alpha one gizmo", [r.text for r in found])
        self.assertTrue(found)

    def test_the_damaged_row_may_be_the_first_one(self):
        self.damage(self.a, 0)                                       # lowest chunk id: used to decide the dimension
        self.assertTrue(self.search("vector"))

    def test_the_over_budget_scan_path_skips_it_too(self):
        self.damage(self.a, 0)
        with patch.dict(os.environ, {"MAGNITE_RAG_INDEX_MB": "0"}):
            self.store._invalidate()
            self.assertTrue(any("bravo" in r.text for r in self.search("vector")))

    def test_ingest_still_works_after_damage(self):
        self.damage(self.a, 0)
        self.add("c", ["charlie only"], [unit(0, 0, 1, 0)])
        self.assertEqual(self.store.search(unit(0, 0, 1, 0), MODEL, top_k=1, mode="vector")[0].text, "charlie only")

    def test_a_real_dimension_mismatch_is_still_an_error(self):
        with self.assertRaises(ValueError) as ctx:
            self.store.search(unit(1, 0, 0, 0, 0, 0), MODEL, mode="vector")
        self.assertIn("dimension", str(ctx.exception))
        with patch.dict(os.environ, {"MAGNITE_RAG_INDEX_MB": "0"}):
            self.store._invalidate()
            with self.assertRaises(ValueError):
                self.store.search(unit(1, 0, 0, 0, 0, 0), MODEL, mode="vector")


class Blocker:
    """Holds the database exclusively from another connection, like another program would."""

    def __init__(self, store):
        self.conn = sqlite3.connect(os.path.join(store._data_dir, "rag.db"), isolation_level=None)

    def __enter__(self):
        self.conn.execute("BEGIN EXCLUSIVE")
        return self

    def __exit__(self, *exc):
        self.conn.execute("ROLLBACK")
        self.conn.close()


class BusyStoreTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.store = RagStore(self.dir)
        self.store.update_config({"model": MODEL})
        self.store.add_document("a", "txt", "", MODEL, ["alpha"], np.stack([unit(1, 0, 0, 0)]), 1.0, 64, 8)
        patcher = patch.dict(os.environ, {"MAGNITE_RAG_DB_TIMEOUT": "0.2"})
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_a_locked_database_raises_store_busy_with_a_clear_message(self):
        with Blocker(self.store):
            for call in (self.store.list_documents, self.store.count_chunks, self.store.get_config, self.store.clear):
                with self.subTest(call=call.__name__):
                    with self.assertRaises(StoreBusy) as ctx:
                        call()
                    self.assertIn("busy", str(ctx.exception).lower())

    def test_the_store_works_again_once_the_lock_is_released(self):
        with Blocker(self.store):
            with self.assertRaises(StoreBusy):
                self.store.list_documents()
        self.assertEqual(len(self.store.list_documents()), 1)

    def test_other_database_errors_are_not_disguised_as_busy(self):
        with patch("backend.rag.store.connection", side_effect=sqlite3.OperationalError("no such table: nope")):
            with self.assertRaises(sqlite3.OperationalError):
                self.store.list_documents()


class BusyRouteTests(IngestTestCase):
    async def test_every_endpoint_answers_503_busy(self):
        with patch.dict(os.environ, {"MAGNITE_RAG_DB_TIMEOUT": "0.2"}), Blocker(self.store):
            for method, path, body in (("GET", "/rag/status", None), ("GET", "/rag/documents", None), ("GET", "/rag/config", None),
                                       ("POST", "/rag/query", {"query": "alpha"}), ("DELETE", "/rag/documents/abc", None),
                                       ("POST", "/rag/clear", None), ("POST", "/rag/config", {"top_k": 3})):
                with self.subTest(path=path):
                    response = await self.client.request(method, path, json=body)
                    self.assertEqual(response.status_code, 503, response.text)
                    self.assertIn("busy", response.json()["detail"].lower())

    async def test_ingest_reports_busy_not_an_embedding_failure(self):
        with patch.dict(os.environ, {"MAGNITE_RAG_DB_TIMEOUT": "0.2"}), Blocker(self.store):
            with self.assertRaises(HTTPException) as ctx:
                await asyncio.to_thread(rag._ingest_document, self.request(64 * 3))
        self.assertEqual(ctx.exception.status_code, 503)
        self.assertIn("busy", ctx.exception.detail.lower())
        self.assertNotIn("embedding failed", ctx.exception.detail.lower())


if __name__ == "__main__":
    unittest.main()
