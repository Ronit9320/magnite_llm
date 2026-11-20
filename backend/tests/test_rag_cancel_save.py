"""Cancelling an ingest while its document is being saved really cancels it (break report 2026-11-20, F3).

Before: the only cancel check sat before the saving phase, so a document whose save was already running
was stored anyway while the UI reported "Cancelled. Nothing was stored."
"""
import asyncio
import shutil
import tempfile
import threading
import unittest
from unittest.mock import patch

import numpy as np
from fastapi import HTTPException

from backend.rag.batching import IngestCancelled
from backend.rag.store import RagStore
from backend.routes import rag

from test_rag_large_ingest import IngestTestCase

MODEL = "all-MiniLM-L6-v2"


class FlipAfter(threading.Event):
    """An event that reads as unset for the first ``polls`` checks, then as set."""

    def __init__(self, polls: int):
        super().__init__()
        self.left = polls

    def is_set(self):
        self.left -= 1
        return self.left < 0


class StoreSaveCancelTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.store = RagStore(self.dir)
        self.store.update_config({"model": MODEL})
        self.n = 5000
        self.chunks = [f"chunk {i} about zebras" for i in range(self.n)]
        self.vectors = np.ones((self.n, 4), np.float32) / 2.0

    def add(self, **kw):
        return self.store.add_document("d", "txt", "", MODEL, self.chunks, self.vectors, 1.0, 64, 8, **kw)

    def test_cancel_during_the_save_rolls_everything_back(self):
        with self.assertRaises(IngestCancelled):
            self.add(cancelled=FlipAfter(2))
        self.assertEqual(self.store.list_documents(), [])
        self.assertEqual(self.store.count_chunks(), 0)
        self.assertEqual(self.store.search(self.vectors[0], MODEL, query_text="zebras", mode="hybrid"), [])

    def test_the_library_is_fully_usable_after_a_cancelled_save(self):
        with self.assertRaises(IngestCancelled):
            self.add(cancelled=FlipAfter(2))
        self.chunks = ["a fresh quokka sentence"]
        self.vectors = np.ones((1, 4), np.float32) / 2.0
        self.add()
        found = self.store.search(self.vectors[0], MODEL, query_text="quokka", mode="hybrid")
        self.assertEqual([r.text for r in found], ["a fresh quokka sentence"])
        self.assertEqual(found[0].match, "both")                  # the keyword index is in sync too

    def test_an_unset_event_saves_normally(self):
        self.add(cancelled=threading.Event())
        self.assertEqual(self.store.count_chunks(), self.n)


class RouteSaveCancelTests(IngestTestCase):
    async def test_cancel_when_the_save_starts_stores_nothing(self):
        cancelled = threading.Event()
        original = RagStore.add_document

        def save_after_cancel(store, *args, **kw):
            cancelled.set()
            return original(store, *args, **kw)

        with patch.object(RagStore, "add_document", save_after_cancel):
            with self.assertRaises(HTTPException) as ctx:
                await asyncio.to_thread(rag._ingest_document, self.request(64 * 40), cancelled)
        self.assertEqual(ctx.exception.status_code, 499)
        self.assertEqual(self.store.list_documents(), [])
        self.assertEqual(self.store.count_chunks(), 0)

    async def test_cancel_that_lands_during_the_commit_is_undone(self):
        cancelled = threading.Event()
        original = RagStore.add_document

        def cancel_after_commit(store, *args, **kw):
            record = original(store, *args, **kw)
            cancelled.set()                                       # too late for the store to notice
            return record

        with patch.object(RagStore, "add_document", cancel_after_commit):
            with self.assertRaises(HTTPException) as ctx:
                await asyncio.to_thread(rag._ingest_document, self.request(64 * 40), cancelled)
        self.assertEqual(ctx.exception.status_code, 499)
        self.assertEqual(self.store.list_documents(), [])
        self.assertEqual(self.store.count_chunks(), 0)

    async def test_uncancelled_ingest_still_stores(self):
        doc = await asyncio.to_thread(rag._ingest_document, self.request(64 * 5), threading.Event())
        self.assertEqual(self.store.count_chunks(), doc.chunks)


if __name__ == "__main__":
    unittest.main()
