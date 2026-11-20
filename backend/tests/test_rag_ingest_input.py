"""What may be ingested: real text only, bounded size, no duplicates, text the model can read.

Break report 2026-11-20: F9 (unreadable text indexed silently), F10 (binary and UTF-16 files),
F11 (no size limit on files), F12 (the same document twice).
"""
import asyncio
import os
import shutil
import tempfile
import unittest
from unittest.mock import patch

import numpy as np
from fastapi import HTTPException

from backend.rag import embeddings
from backend.rag.chunking import decode_text_bytes, extract_text
from backend.rag.store import RagStore
from backend.routes import rag

from test_rag_large_ingest import IngestTestCase, words

MODEL = "all-MiniLM-L6-v2"


class DecodeTextTests(unittest.TestCase):
    def test_utf16_with_a_byte_order_mark_is_decoded(self):
        text = "The quarterly invoice total is overdue."
        self.assertEqual(decode_text_bytes(text.encode("utf-16")), text)
        self.assertEqual(decode_text_bytes(text.encode("utf-16-be").join([b"\xfe\xff", b""])), text)

    def test_utf8_with_and_without_a_byte_order_mark(self):
        self.assertEqual(decode_text_bytes("café 日本".encode("utf-8-sig")), "café 日本")
        self.assertEqual(decode_text_bytes("café".encode("utf-8")), "café")

    def test_legacy_single_byte_text_still_reads(self):
        self.assertEqual(decode_text_bytes(b"caf\xe9 au lait\r\nsecond line"), "café au lait\r\nsecond line")

    def test_binary_content_is_refused_with_a_clear_message(self):
        samples = {"png": b"\x89PNG\r\n\x1a\n" + bytes(range(256)) * 8, "zip": b"PK\x03\x04" + b"\x00" * 100 + b"abc",
                   "nul-late": b"hello world " * 1000 + b"\x00" + b"tail", "random": bytes((i * 37) % 256 for i in range(4000))}
        for name, data in samples.items():
            with self.subTest(name):
                with self.assertRaises(ValueError) as ctx:
                    decode_text_bytes(data)
                self.assertIn("binary", str(ctx.exception).lower())

    def test_extract_text_reads_files_through_the_same_check(self):
        with tempfile.TemporaryDirectory() as tmp:
            good, bad = os.path.join(tmp, "a.txt"), os.path.join(tmp, "b.docx")
            open(good, "wb").write("Hello é".encode("utf-16"))
            open(bad, "wb").write(b"PK\x03\x04" + b"\x00" * 50)
            self.assertEqual(extract_text(good, "txt"), "Hello é")
            with self.assertRaises(ValueError):
                extract_text(bad, "txt")


class FileIngestTests(IngestTestCase):
    def write(self, name, data):
        path = os.path.join(self.tmp.name, name)
        open(path, "wb").write(data)
        return path

    async def asyncSetUp(self):
        await super().asyncSetUp()
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    async def ingest_path(self, path):
        return await asyncio.to_thread(rag._ingest_document, rag.RagIngestRequest(path=path, chunk_size=64, chunk_overlap=0))

    async def test_a_binary_file_is_a_400_and_nothing_is_embedded(self):
        path = self.write("scan.docx", b"PK\x03\x04" + os.urandom(3000))
        with self.assertRaises(HTTPException) as ctx:
            await self.ingest_path(path)
        self.assertEqual(ctx.exception.status_code, 400)
        self.assertIn("binary", ctx.exception.detail.lower())
        self.assertEqual(self.calls, [])

    async def test_a_utf16_file_is_indexed_as_readable_text(self):
        path = self.write("u16.txt", ("The invoice is overdue. " * 20).encode("utf-16"))
        await self.ingest_path(path)
        with self.store._conn() as conn:
            texts = [r[0] for r in conn.execute("SELECT text FROM rag_chunks")]
        self.assertTrue(texts and all("\x00" not in t and "invoice" in t for t in texts))

    async def test_a_file_over_the_size_limit_is_a_413_before_it_is_read(self):
        path = self.write("big.txt", b"word " * 100)
        with patch.object(rag, "MAX_FILE_BYTES", 100):
            with self.assertRaises(HTTPException) as ctx:
                await self.ingest_path(path)
        self.assertEqual(ctx.exception.status_code, 413)
        self.assertIn("too large", ctx.exception.detail.lower())

    async def test_text_that_expands_beyond_the_character_limit_is_a_413(self):
        path = self.write("ok.txt", b"word " * 100)
        with patch.object(rag, "MAX_CONTENT_CHARS", 50):
            with self.assertRaises(HTTPException) as ctx:
                await self.ingest_path(path)
        self.assertEqual(ctx.exception.status_code, 413)


class DuplicateTests(IngestTestCase):
    async def test_the_same_document_twice_is_a_409_naming_the_original(self):
        first = await asyncio.to_thread(rag._ingest_document, self.request(64 * 10, name="policy.txt"))
        calls_before = len(self.calls)
        with self.assertRaises(HTTPException) as ctx:
            await asyncio.to_thread(rag._ingest_document, self.request(64 * 10, name="copy-of-policy.txt"))
        self.assertEqual(ctx.exception.status_code, 409)
        self.assertIn("already in Knowledge", ctx.exception.detail)
        self.assertIn("policy.txt", ctx.exception.detail)
        self.assertEqual(len(self.calls), calls_before)            # rejected before any embedding work
        self.assertEqual(self.store.count_chunks(), first.chunks)

    async def test_a_different_document_is_accepted(self):
        await asyncio.to_thread(rag._ingest_document, self.request(64 * 10, name="a.txt"))
        other = rag.RagIngestRequest(name="b.txt", content=words(64 * 10 + 5).replace("w0 ", "other "),
                                     chunk_size=64, chunk_overlap=0)
        await asyncio.to_thread(rag._ingest_document, other)
        self.assertEqual(len(self.store.list_documents()), 2)

    async def test_a_removed_document_can_be_added_again(self):
        first = await asyncio.to_thread(rag._ingest_document, self.request(64 * 10, name="a.txt"))
        self.store.delete_document(first.id)
        await asyncio.to_thread(rag._ingest_document, self.request(64 * 10, name="a.txt"))
        self.assertEqual(len(self.store.list_documents()), 1)


class DuplicateStoreTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.store = RagStore(self.dir)
        self.store.update_config({"model": MODEL})
        self.chunks = ["first chunk", "middle chunk", "last chunk"]
        self.store.add_document("orig", "txt", "", MODEL, self.chunks, np.ones((3, 4), np.float32) / 2.0, 1.0, 64, 8)

    def test_identical_chunks_match(self):
        self.assertEqual(self.store.find_duplicate(list(self.chunks)).name, "orig")

    def test_any_difference_is_not_a_duplicate(self):
        for other in (["first chunk", "middle chunk"], ["first chunk", "MIDDLE chunk", "last chunk"],
                      ["x", "middle chunk", "last chunk"], ["first chunk", "middle chunk", "x"],
                      ["first chunk", "middle chunk", "last chunk", "more"]):
            with self.subTest(other=other):
                self.assertIsNone(self.store.find_duplicate(other))

    def test_empty_library_and_empty_chunks(self):
        self.store.clear()
        self.assertIsNone(self.store.find_duplicate(list(self.chunks)))
        self.assertIsNone(self.store.find_duplicate([]))


class UnreadableTextTests(IngestTestCase):
    async def test_text_the_model_cannot_read_is_a_422_after_the_first_batch(self):
        with patch.object(embeddings, "unknown_ratio", return_value=0.93):
            with self.assertRaises(HTTPException) as ctx:
                await asyncio.to_thread(rag._ingest_document, self.request(64 * 40))
        self.assertEqual(ctx.exception.status_code, 422)
        self.assertIn("cannot read", ctx.exception.detail)
        self.assertIn("93%", ctx.exception.detail)
        self.assertEqual(len(self.calls), 1)                       # stopped at once, not after 40 chunks
        self.assertEqual(self.store.count_chunks(), 0)

    async def test_mostly_cjk_text_is_accepted_because_keyword_search_covers_it(self):
        zh = rag.RagIngestRequest(name="zh.txt", content="今天我们讨论了公司的季度财报。" * 200, chunk_size=64, chunk_overlap=0)
        with patch.object(embeddings, "unknown_ratio", return_value=0.93):
            await asyncio.to_thread(rag._ingest_document, zh)
        self.assertEqual(len(self.store.list_documents()), 1)

    async def test_mostly_readable_text_passes(self):
        with patch.object(embeddings, "unknown_ratio", return_value=0.2):
            await asyncio.to_thread(rag._ingest_document, self.request(64 * 3))
        self.assertEqual(len(self.store.list_documents()), 1)

    async def test_a_model_without_a_tokenizer_is_not_blocked(self):
        with patch.object(embeddings, "unknown_ratio", return_value=None):
            await asyncio.to_thread(rag._ingest_document, self.request(64 * 3))
        self.assertEqual(len(self.store.list_documents()), 1)


class UnknownRatioTests(unittest.TestCase):
    def test_embedder_counts_unknown_tokens_without_special_tokens(self):
        class Tok:
            unk_token_id = 100

            def __call__(self, texts, add_special_tokens=True, **kw):
                return {"input_ids": [[100 if ch == "?" else 5 for ch in t.replace(" ", "")] for t in texts]}

        emb = embeddings.Embedder(embeddings.EmbedderSpec("m", "m", 256, 4))
        emb._encoder = type("E", (), {"tokenizer": Tok(), "max_seq_length": 256})()
        self.assertAlmostEqual(emb.unknown_token_ratio(["ab??", "????"]), 6 / 8)
        self.assertEqual(emb.unknown_token_ratio([]), 0.0)

    def test_none_when_there_is_no_unknown_token_id(self):
        emb = embeddings.Embedder(embeddings.EmbedderSpec("m", "m", 256, 4))
        emb._encoder = type("E", (), {"tokenizer": lambda *a, **k: {"input_ids": [[1]]}, "max_seq_length": 256})()
        self.assertIsNone(emb.unknown_token_ratio(["x"]))

    def test_module_function_is_none_when_the_model_is_not_loaded(self):
        embeddings._reset_for_tests()
        self.assertIsNone(embeddings.unknown_ratio(["x"], "some/model"))


if __name__ == "__main__":
    unittest.main()
