"""The Low findings of the 2026-11-20 break report (F15-F25).

F15 config validation and same-model relabel, F16 chunk size for token-dense text, F17 name/path length,
F18 non-finite vectors, F19 JSON NaN/Infinity, F20 lone surrogates, F21 markdown cleaning,
F23 literal identifiers, F24 chunk weight bound, F25 store.search argument checks.
"""
import asyncio
import random
import shutil
import tempfile
import unittest
from unittest.mock import patch

import numpy as np
from fastapi import HTTPException

from backend.rag import context, embeddings
from backend.rag.batching import embed_batches
from backend.rag.chunking import _weight, chunk_text, extract_content
from backend.rag.store import RagStore
from backend.routes import rag

from test_rag_large_ingest import IngestTestCase, words

MODEL = "all-MiniLM-L6-v2"
FULL_MINILM = "sentence-transformers/all-MiniLM-L6-v2"
JSON = {"content-type": "application/json"}


class RawJsonCase(IngestTestCase):
    async def post_raw(self, path, raw):
        return await self.client.post(path, content=raw, headers=JSON)


class JsonEdgeValueTests(RawJsonCase):
    async def test_non_finite_numbers_are_a_422_not_a_500(self):
        cases = (("/rag/query", b'{"query":"x","min_score":NaN}'), ("/rag/query", b'{"query":"x","min_score":Infinity}'),
                 ("/rag/query", b'{"query":"x","top_k":-Infinity}'),
                 ("/rag/documents", b'{"content":"hi there","name":"a","chunk_size":1e400}'),
                 ("/rag/config", b'{"min_score":NaN}'))
        for path, raw in cases:
            with self.subTest(raw=raw):
                response = await self.post_raw(path, raw)
                self.assertEqual(response.status_code, 422, response.text)
                self.assertIn("detail", response.json())


class SurrogateTests(RawJsonCase):
    async def test_a_lone_surrogate_is_a_400_everywhere_it_can_arrive(self):
        cases = (("/rag/query", b'{"query":"hi \\ud800 there"}'), ("/rag/documents", b'{"path":"x\\ud800y"}'),
                 ("/rag/documents", b'{"name":"a\\ud800","content":"hello world sentence one."}'),
                 ("/rag/documents", b'{"name":"a.txt","kind":"x\\ud800","content":"hello world. second."}'))
        for path, raw in cases:
            with self.subTest(raw=raw):
                response = await self.post_raw(path, raw)
                self.assertEqual(response.status_code, 400, response.text)
                self.assertIn("invalid character", response.json()["detail"])
        self.assertEqual(self.calls, [])                          # rejected before any embedding work

    async def test_chat_retrieval_rejects_it_too(self):
        with self.assertRaises(ValueError) as ctx:
            context.retrieve_chunks("bad \ud800 text", None, None, None)
        self.assertIn("invalid character", str(ctx.exception))


class NoSourcesSelectedTests(IngestTestCase):
    async def test_an_empty_source_list_says_so_instead_of_blaming_the_threshold(self):
        self.store.add_document("d", "txt", "", self.store.get_config().model, ["some text"], np.ones((1, 4), np.float32) / 2.0, 1.0, 64, 8)
        with self.assertRaises(ValueError) as ctx:
            context.retrieve_chunks("anything", None, None, [])
        self.assertIn("No Knowledge sources are turned on", str(ctx.exception))
        self.assertNotIn("threshold", str(ctx.exception))


class LengthCapTests(IngestTestCase):
    async def test_name_and_path_lengths_are_capped(self):
        ok = rag.RagIngestRequest(name="n" * 255, content=words(64 * 2), chunk_size=64, chunk_overlap=0)
        await asyncio.to_thread(rag._ingest_document, ok)
        for body in (rag.RagIngestRequest(name="n" * 256, content="hello there. second sentence."),
                     rag.RagIngestRequest(path="/" + "p" * 4096)):
            with self.subTest(name_len=len(body.name), path_len=len(body.path)):
                with self.assertRaises(HTTPException) as ctx:
                    await asyncio.to_thread(rag._ingest_document, body)
                self.assertEqual(ctx.exception.status_code, 400)
                self.assertIn("too long", ctx.exception.detail)


class FiniteVectorTests(IngestTestCase):
    async def test_an_embedder_returning_nan_fails_the_ingest_and_stores_nothing(self):
        with patch.object(embeddings, "embed_documents", side_effect=lambda t, m, d=0: np.full((len(t), 4), np.nan, np.float32)):
            with self.assertRaises(HTTPException) as ctx:
                await asyncio.to_thread(rag._ingest_document, self.request(64 * 4))
        self.assertIn("invalid numbers", ctx.exception.detail)
        self.assertEqual(self.store.count_chunks(), 0)

    def test_embed_batches_rejects_non_finite_vectors(self):
        bad = np.ones((2, 3), np.float32)
        bad[1, 1] = np.inf
        with self.assertRaises(ValueError) as ctx:
            embed_batches([["a", "b"]], lambda texts: bad, lambda offset, v: None)
        self.assertIn("invalid numbers", str(ctx.exception))

    def test_the_store_refuses_them_as_well(self):
        directory = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, directory, ignore_errors=True)
        store = RagStore(directory)
        store.update_config({"model": MODEL})
        vecs = np.ones((1, 4), np.float32)
        vecs[0, 2] = np.nan
        with self.assertRaises(ValueError):
            store.add_document("d", "txt", "", MODEL, ["x"], vecs, 1.0, 64, 8)
        self.assertEqual(store.count_chunks(), 0)

    def test_a_reindex_that_produces_nan_changes_nothing(self):
        directory = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, directory, ignore_errors=True)
        store = RagStore(directory)
        store.update_config({"model": MODEL})
        store.add_document("d", "txt", "", MODEL, ["a", "b"], np.ones((2, 4), np.float32) / 2.0, 1.0, 64, 8)
        with self.assertRaises(ValueError):
            store.reindex("other", 0, lambda texts: np.full((len(texts), 4), np.nan, np.float32))
        self.assertEqual(store.get_config().model, MODEL)


class MiniLMCase(IngestTestCase):
    """An empty library configured with the real model name, so its limits and capabilities apply."""

    async def asyncSetUp(self):
        await super().asyncSetUp()
        self.store.update_config({"model": MODEL})


class ConfigValidationTests(MiniLMCase):
    async def test_truncation_on_a_model_that_cannot_truncate_is_a_400(self):
        response = await self.client.post("/rag/config", json={"truncate_dim": 128})
        self.assertEqual(response.status_code, 400, response.text)
        self.assertIn("does not support", response.json()["detail"])
        self.assertEqual(self.store.get_config().truncate_dim, 0)

    async def test_truncation_is_fine_on_a_model_that_supports_it(self):
        response = await self.client.post("/rag/config", json={"model": "Qwen/Qwen3-Embedding-0.6B", "truncate_dim": 256})
        self.assertEqual(response.status_code, 200, response.text)

    async def test_switching_to_another_spelling_of_the_same_model_is_a_relabel(self):
        await asyncio.to_thread(rag._ingest_document, self.request(64 * 4))
        calls_before = len(self.calls)
        response = await self.client.post("/rag/reindex", json={"model": FULL_MINILM})
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(len(self.calls), calls_before)            # nothing was re-embedded
        self.assertEqual(self.store.get_config().model, FULL_MINILM)
        found = self.store.search(np.ones(4, np.float32) / 2.0, FULL_MINILM, top_k=2, mode="vector")
        self.assertTrue(found)

    async def test_the_config_route_accepts_the_other_spelling_too(self):
        await asyncio.to_thread(rag._ingest_document, self.request(64 * 4))
        response = await self.client.post("/rag/config", json={"model": FULL_MINILM})
        self.assertEqual(response.status_code, 200, response.text)
        self.assertEqual(self.store.get_config().model, FULL_MINILM)
        response = await self.client.post("/rag/config", json={"model": "BAAI/bge-m3"})
        self.assertEqual(response.status_code, 409)               # a different model still needs a reindex


class MarkdownTests(unittest.TestCase):
    def words(self, source):
        return extract_content(source, "md").split()

    def test_noise_is_removed(self):
        cases = {
            "![logo](a.png) Intro": ["logo", "Intro"],
            "See [the docs][1] and [other][] now.\n\n[1]: https://example.com/docs\n[other]: <https://x.y>": ["See", "the", "docs", "and", "other", "now."],
            "---\ntitle: X\ntags: [a]\n---\nBody text.": ["Body", "text."],
            "Hello <b>bold</b> <br/> world": ["Hello", "bold", "world"],
            "| a | b |\n|---|:-:|\n| 1 | 2 |": ["a", "b", "1", "2"],
        }
        for source, expected in cases.items():
            with self.subTest(source=source):
                self.assertEqual(self.words(source), expected)

    def test_code_and_symbols_are_left_alone(self):
        for source in ("List<String> names = new ArrayList<String>();", "def f(**kwargs): return __init__", "a | b | c in a shell pipeline",
                       "if (a < b && c > d) { }", "[not a link] and ---", "x --- y"):
            with self.subTest(source=source):
                self.assertEqual(self.words(source), source.split())

    def test_a_horizontal_rule_is_not_front_matter(self):
        self.assertEqual(self.words("Intro\n\n---\n\nMore text"), ["Intro", "---", "More", "text"])


class LiteralIdentifierTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.store = RagStore(self.dir)
        self.store.update_config({"model": MODEL})
        rng = np.random.default_rng(2)
        self.q = np.eye(1, 8, dtype=np.float32)[0]
        filler = rng.standard_normal((30, 8)).astype(np.float32)
        filler /= np.linalg.norm(filler, axis=1, keepdims=True)
        self.store.add_document("filler", "txt", "", MODEL, [f"filler passage {i} about planning" for i in range(30)], filler, 1.0, 64, 8)

    def top(self, query, target, distractors):
        # the distractors are closer to the query by meaning, so only the literal text can put the target first
        vecs = np.stack([-np.eye(1, 8, 0, dtype=np.float32)[0]] + [np.eye(1, 8, 0, dtype=np.float32)[0] * 0.9 + 0.1 * np.eye(1, 8, 1, dtype=np.float32)[0]
                                                                  for _ in distractors])
        doc = self.store.add_document("t-" + query, "txt", "", MODEL, [target] + distractors, vecs / np.linalg.norm(vecs, axis=1, keepdims=True),
                                      1.0, 64, 8)
        found = self.store.search(self.q, MODEL, top_k=3, query_text=query, mode="hybrid")
        self.store.delete_document(doc.id)
        return found

    def test_the_chunk_containing_the_literal_identifier_ranks_first(self):
        cases = {
            "C++": ("We compile the engine with C++ and Rust.", ["C# and Java and C interop notes.", "net income c rate"]),
            ".NET": ("The service runs on .NET 8 hosting.", ["net income grew; net profit; net sales"]),
            "INV-2024-0031": ("Invoice INV-2024-0031 is overdue.", ["Invoice INV-2024-0032 is overdue.", "Invoice INV-2023-0031 is paid."]),
            "user@example.com": ("Contact user@example.com for access.", ["Contact other@example.com for access."]),
        }
        for query, (target, distractors) in cases.items():
            with self.subTest(query):
                self.assertEqual(self.top(query, target, distractors)[0].text, target)

    def test_plain_word_queries_are_ordered_as_before(self):
        found = self.top("zebra", "an unrelated chunk", ["zebra notes for the quarter"])
        self.assertEqual(found[0].text, "zebra notes for the quarter")

    def test_the_reported_fused_score_is_still_the_two_leg_value(self):
        found = self.top("INV-77341", "the invoice INV-77341 is overdue", ["a distractor about nothing"])
        hit = next(r for r in found if "INV-77341" in r.text)           # found by the keyword leg only
        self.assertEqual(hit.fused_score, round(1 / 61, 6))


class ChunkWeightTests(unittest.TestCase):
    def test_no_chunk_is_heavier_than_the_size_it_was_asked_for(self):
        rng = random.Random(1234)
        frags = ["word", "The", "quick", "brown.", "fox!", "jumps?", "over\n", "\n\n", "lazy,", "日本語テキスト", "한국어", "😀", "x" * 40,
                 "https://example.com/" + "a" * 60, "CamelCaseIdentifierThatIsLong", "(paren)", "\"quoted.\"", "é", "  ", "\t",
                 "ǅ", "İstanbul", "naïve", "1,234.56", "a.b.c.d", "...", "—", "Mr.", "etc."]
        for run in range(1500):
            text = "".join(rng.choice(frags) + rng.choice([" ", " ", "", "\n", "  "]) for _ in range(rng.randint(1, 400)))
            size = rng.randint(8, 120)
            overlap = rng.randint(0, size - 1)
            for chunk in chunk_text(text, size, overlap):
                heaviest = sum(_weight(t) for t in chunk.split())
                self.assertLessEqual(heaviest, size, f"run {run}: {chunk[:80]!r}")


class DenseTextChunkSizeTests(MiniLMCase):
    async def test_url_heavy_text_is_not_chunked_to_one_url_per_chunk(self):
        text = " ".join(f"https://example.com/path/a1b2c3/d4e5f?id={i}&ref=xyz" for i in range(500))
        with patch.object(embeddings, "count_tokens", side_effect=lambda texts, model: [len(t) // 3 for t in texts]):
            await asyncio.to_thread(rag._ingest_document, rag.RagIngestRequest(name="urls.txt", content=text))
        with self.store._conn() as conn:
            lengths = [len(r[0]) // 3 for r in conn.execute("SELECT text FROM rag_chunks")]
        self.assertLessEqual(max(lengths), 256)                    # still inside the model's limit
        self.assertGreaterEqual(sum(lengths) / len(lengths), 150)  # and not wastefully small
        self.assertLessEqual(len(lengths), 80)


class SearchArgumentTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.store = RagStore(self.dir)
        self.store.update_config({"model": MODEL})
        n = 600
        self.store.add_document("d", "txt", "", MODEL, [f"chunk {i}" for i in range(n)], np.ones((n, 4), np.float32) / 2.0, 1.0, 64, 8)
        self.q = np.ones(4, np.float32) / 2.0

    def test_top_k_below_one_is_an_error(self):
        for top_k in (0, -5):
            with self.subTest(top_k=top_k), self.assertRaises(ValueError):
                self.store.search(self.q, MODEL, top_k=top_k)

    def test_a_huge_top_k_is_capped(self):
        self.assertEqual(len(self.store.search(self.q, MODEL, top_k=10 ** 9, mode="vector")), 500)

    def test_min_score_must_be_a_number_in_range(self):
        for bad in (float("nan"), float("inf")):
            with self.subTest(min_score=bad), self.assertRaises(ValueError):
                self.store.search(self.q, MODEL, min_score=bad)
        self.assertEqual(self.store.search(self.q, MODEL, min_score=2.0), [])      # above the cosine range: nothing qualifies


if __name__ == "__main__":
    unittest.main()
