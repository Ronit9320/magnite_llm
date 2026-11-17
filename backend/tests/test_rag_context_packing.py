import shutil
import tempfile
import unittest
from unittest.mock import patch

import numpy as np

from backend.inference.schemas import ChatMessage, ChatRequest, ChatRole
from backend.rag import context
from backend.rag.context import injectable_context, pack_context, retrieve_chunks
from backend.rag.schemas import RagChunk
from backend.rag.store import RagStore
from backend.routes import inference

HEADER = ("The user has enabled retrieval over their local documents. Prefer "
          "these retrieved passages when answering:")


def chunk(text, index=0, name="notes.txt"):
    return RagChunk(document_id="d1", document_name=name, index=index, text=text,
                    tokens=len(text.split()), score=0.9)


def sentences(prefix, count=6):
    return " ".join(f"{prefix} sentence number {i} says something useful." for i in range(count))


def budget(n):
    return lambda text: len(text) <= n


def piece(i, c):
    return f"\n\n[{i}] (from {c.document_name}) {c.text}"


class PackContextTests(unittest.TestCase):
    def setUp(self):
        self.chunks = [chunk(sentences(p), i) for i, p in enumerate(("alpha", "bravo", "charlie"))]

    def test_everything_included_when_it_all_fits(self):
        text, used = pack_context(self.chunks, budget(10_000))
        self.assertEqual(used, self.chunks)
        self.assertEqual(text, HEADER + "".join(piece(i, c) for i, c in enumerate(self.chunks, 1)))

    def test_includes_whole_passages_only(self):
        two_and_a_half = len(HEADER) + len(piece(1, self.chunks[0])) + len(piece(2, self.chunks[1])) \
            + len(piece(3, self.chunks[2])) // 2
        text, used = pack_context(self.chunks, budget(two_and_a_half))
        self.assertEqual([c.text for c in used], [self.chunks[0].text, self.chunks[1].text])
        self.assertIn(self.chunks[1].text, text)
        self.assertNotIn("charlie", text)

    def test_numbering_is_contiguous_after_dropping_trailing_passages(self):
        limit = len(HEADER) + len(piece(1, self.chunks[0])) + len(piece(2, self.chunks[1]))
        text, used = pack_context(self.chunks, budget(limit))
        self.assertEqual(len(used), 2)
        self.assertIn("[1] (from notes.txt)", text)
        self.assertIn("[2] (from notes.txt)", text)
        self.assertNotIn("[3]", text)

    def test_stops_at_first_passage_that_does_not_fit_even_if_a_later_one_would(self):
        small = chunk("tiny passage.", 5)
        limit = len(HEADER) + len(piece(1, self.chunks[0])) + len(piece(2, small)) - 1
        text, used = pack_context([self.chunks[0], self.chunks[1], small], budget(limit))
        self.assertEqual([c.index for c in used], [0])       # rank order is preserved, no skipping

    def test_first_passage_trimmed_at_sentence_boundary(self):
        first = chunk(sentences("alpha", 8), 0)
        limit = len(HEADER) + len("\n\n[1] (from notes.txt) ") + len(first.text) // 2
        text, used = pack_context([first, self.chunks[1]], budget(limit))
        self.assertEqual(len(used), 1)
        trimmed = used[0].text
        self.assertTrue(trimmed.endswith("."))
        self.assertTrue(first.text.startswith(trimmed))
        self.assertLess(len(trimmed), len(first.text))
        self.assertTrue(text.endswith(trimmed))              # the citation is exactly what the model saw
        self.assertLessEqual(len(text), limit)

    def test_trims_at_whitespace_when_there_is_no_sentence_end(self):
        first = chunk("word " * 200, 0)
        limit = len(HEADER) + len("\n\n[1] (from notes.txt) ") + 101
        text, used = pack_context([first], budget(limit))
        self.assertTrue(used[0].text.endswith("word"))       # never mid-word
        self.assertLessEqual(len(text), limit)

    def test_trims_unbroken_text_as_a_last_resort(self):
        first = chunk("x" * 500, 0)
        limit = len(HEADER) + len("\n\n[1] (from notes.txt) ") + 200
        text, used = pack_context([first], budget(limit))
        self.assertEqual(used[0].text, "x" * 200)

    def test_no_room_raises_when_budget_is_under_min_chars(self):
        with self.assertRaises(ValueError) as ctx:
            pack_context(self.chunks, budget(100))
        self.assertEqual(str(ctx.exception), "No room for retrieved context. Start a new chat or reduce max tokens.")

    def test_no_room_raises_when_header_fits_but_no_passage_does(self):
        with self.assertRaises(ValueError):
            pack_context(self.chunks, budget(len(HEADER) + 10))

    def test_never_returns_a_one_character_context(self):
        for limit in range(0, 160):
            try:
                text, _ = pack_context([chunk("Hello there. " * 40)], budget(limit))
            except ValueError:
                continue
            self.assertGreaterEqual(len(text), 128, limit)

    def test_trimmed_first_passage_is_a_copy_not_a_mutation(self):
        first = chunk(sentences("alpha", 8), 0)
        original = first.text
        pack_context([first], budget(len(HEADER) + 200))
        self.assertEqual(first.text, original)

    def test_empty_chunk_list_raises_no_room(self):
        with self.assertRaises(ValueError):
            pack_context([], budget(10_000))


class StubEngine:
    def context_size(self):
        return 4096

    def count_messages(self, messages):
        return sum(len(m.content) for m in messages)


class WithRagContextTests(unittest.TestCase):
    def request(self, max_tokens=256):
        return ChatRequest(messages=[ChatMessage(role="user", content="question?")],
                           max_tokens=max_tokens, rag={"enabled": True})

    def test_citations_match_the_packed_text(self):
        chunks = [chunk(sentences("alpha"), 0, "a.txt"), chunk(sentences("bravo"), 1, "b.txt")]
        citations = []
        with patch.object(inference, "retrieve_chunks", return_value=chunks):
            messages = inference._with_rag_context(self.request(), StubEngine(), citations)
        self.assertEqual(messages[0].role, ChatRole.system)
        self.assertEqual(citations, [
            {"document_name": "a.txt", "index": 0, "text": chunks[0].text},
            {"document_name": "b.txt", "index": 1, "text": chunks[1].text},
        ])
        for c in citations:
            self.assertIn(c["text"], messages[0].content)

    def test_only_cited_passages_are_in_the_prompt(self):
        chunks = [chunk(sentences("alpha"), 0), chunk("zzz " * 1500, 1)]
        citations = []
        with patch.object(inference, "retrieve_chunks", return_value=chunks):
            messages = inference._with_rag_context(self.request(max_tokens=1), StubEngine(), citations)
        self.assertEqual(len(citations), 1)
        self.assertNotIn("zzz", messages[0].content)
        self.assertLessEqual(StubEngine().count_messages(messages), 4096)

    def test_budget_is_respected_for_a_single_huge_passage(self):
        huge = chunk("Short sentence here. " * 1000, 0)
        citations = []
        with patch.object(inference, "retrieve_chunks", return_value=[huge]):
            messages = inference._with_rag_context(self.request(), StubEngine(), citations)
        self.assertLessEqual(StubEngine().count_messages(messages) + 256, 4096)
        self.assertTrue(citations[0]["text"].endswith("."))
        self.assertTrue(messages[0].content.endswith(citations[0]["text"]))

    def test_no_room_error_surfaces(self):
        engine = StubEngine()
        engine.context_size = lambda: 300
        with patch.object(inference, "retrieve_chunks", return_value=[chunk("hello world. " * 50)]):
            with self.assertRaises(ValueError) as ctx:
                inference._with_rag_context(self.request(max_tokens=256), engine, [])
        self.assertIn("No room", str(ctx.exception))

    def test_rag_disabled_returns_messages_unchanged(self):
        req = ChatRequest(messages=[ChatMessage(role="user", content="hi")], max_tokens=10)
        self.assertEqual(inference._with_rag_context(req, StubEngine()), req.messages)

    def test_conversation_over_budget_is_rejected_before_retrieval(self):
        engine = StubEngine()
        engine.context_size = lambda: 5
        with patch.object(inference, "retrieve_chunks", side_effect=AssertionError("must not retrieve")):
            with self.assertRaises(ValueError):
                inference._with_rag_context(self.request(max_tokens=10), engine, [])


class RetrieveChunksTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.dir, ignore_errors=True)
        self.store = RagStore(self.dir)
        self.doc = self.store.add_document("facts.txt", "txt", "", "all-MiniLM-L6-v2",
                                           ["Paris has the Eiffel Tower."], np.array([[1.0, 0.0]], np.float32),
                                           1.0, 64, 8)

    def patches(self):
        return (patch.object(context, "get_rag_store", return_value=self.store),
                patch.object(context, "is_available", return_value=True),
                patch.object(context, "embed_texts", return_value=np.array([[1.0, 0.0]], np.float32)))

    def run_retrieve(self, **kwargs):
        a, b, c = self.patches()
        with a, b, c:
            return retrieve_chunks("Paris", **{"top_k": None, "min_score": None, "document_ids": None, **kwargs})

    def test_returns_ranked_chunks(self):
        self.assertEqual([c.text for c in self.run_retrieve()], ["Paris has the Eiffel Tower."])

    def test_empty_document_selection_reports_no_matches(self):
        with self.assertRaises(ValueError) as ctx:
            self.run_retrieve(document_ids=[])
        self.assertIn("No matching document passages", str(ctx.exception))

    def test_unavailable_engine_reports_clearly(self):
        with patch.object(context, "get_rag_store", return_value=self.store), \
                patch.object(context, "is_available", return_value=False):
            with self.assertRaises(RuntimeError) as ctx:
                retrieve_chunks("Paris", None, None, None)
        self.assertIn("RAG is unavailable", str(ctx.exception))

    def test_empty_index_reports_clearly(self):
        self.store.clear()
        with self.assertRaises(ValueError) as ctx:
            self.run_retrieve()
        self.assertIn("No RAG documents are indexed", str(ctx.exception))

    def test_injectable_context_wrapper_still_returns_header_and_passage(self):
        a, b, c = self.patches()
        with a, b, c:
            text = injectable_context("Paris")
        self.assertTrue(text.startswith(HEADER))
        self.assertIn("[1] (from facts.txt) Paris has the Eiffel Tower.", text)


if __name__ == "__main__":
    unittest.main()
