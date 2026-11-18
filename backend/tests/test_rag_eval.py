import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "evals"))

import rag_eval  # noqa: E402


class IsHitTests(unittest.TestCase):
    def test_is_hit_normalizes_case_and_whitespace(self):
        self.assertTrue(rag_eval.is_hit("The  Total\nDue is\t$1,200.", ["total due is $1,200"]))

    def test_is_hit_requires_all_needles(self):
        self.assertFalse(rag_eval.is_hit("alpha beta", ["alpha", "gamma"]))
        self.assertTrue(rag_eval.is_hit("alpha beta gamma", ["alpha", "gamma"]))

    def test_is_hit_empty_needles_is_never_a_hit(self):
        self.assertFalse(rag_eval.is_hit("anything", []))


class MetricTests(unittest.TestCase):
    HITS = [[False, True], [True, False], [False, False]]

    def test_recall_at_k_known_values(self):
        self.assertAlmostEqual(rag_eval.recall_at_k(self.HITS, 1), 1 / 3)
        self.assertAlmostEqual(rag_eval.recall_at_k(self.HITS, 2), 2 / 3)

    def test_mrr_known_values(self):
        self.assertAlmostEqual(rag_eval.mrr(self.HITS), (1 / 2 + 1 + 0) / 3)

    def test_metrics_on_empty_input_are_zero(self):
        self.assertEqual(rag_eval.recall_at_k([], 4), 0.0)
        self.assertEqual(rag_eval.mrr([]), 0.0)


class LoadQuestionsTests(unittest.TestCase):
    def _write(self, lines):
        fd, path = tempfile.mkstemp(suffix=".jsonl")
        with os.fdopen(fd, "w", encoding="utf-8") as f:
            f.write("\n".join(lines))
        self.addCleanup(os.remove, path)
        return path

    def test_load_questions_parses_rows_and_skips_blank_lines(self):
        path = self._write([
            json.dumps({"question": "Q1?", "doc": "a.txt", "answer_contains": ["x"]}),
            "",
            json.dumps({"question": "Q2?", "doc": "b.pdf", "answer_contains": ["y", "z"]}),
        ])
        qs = rag_eval.load_questions(path)
        self.assertEqual([q.question for q in qs], ["Q1?", "Q2?"])
        self.assertEqual(qs[1].answer_contains, ["y", "z"])

    def test_load_questions_rejects_missing_fields(self):
        path = self._write([json.dumps({"question": "Q?", "doc": "a.txt"})])
        with self.assertRaises(ValueError) as ctx:
            rag_eval.load_questions(path)
        self.assertIn("line 1", str(ctx.exception))

    def test_load_questions_rejects_empty_needles(self):
        path = self._write([json.dumps({"question": "Q?", "doc": "a", "answer_contains": []})])
        with self.assertRaises(ValueError):
            rag_eval.load_questions(path)

    def test_load_questions_rejects_bad_json(self):
        path = self._write(["{not json"])
        with self.assertRaises(ValueError):
            rag_eval.load_questions(path)


class FirstHitRankTests(unittest.TestCase):
    def test_first_hit_rank_is_one_based_and_zero_for_miss(self):
        self.assertEqual(rag_eval.first_hit_ranks([[False, True], [True, False], [False, False]]), [2, 1, 0])

    def test_split_recall_by_group(self):
        qs = [rag_eval.Question("a", "x.pdf", ["p"]), rag_eval.Question("b", "y.md", ["q"]),
              rag_eval.Question("c", "y.md", ["r"])]
        ranks = [1, 0, 3]
        out = rag_eval.recall_by_doc(qs, ranks, k=4)
        self.assertEqual(out, {"x.pdf": 1.0, "y.md": 0.5})


class KitSupportTests(unittest.TestCase):
    def test_default_candidates_are_the_three_presets(self):
        ids = [c[0] for c in rag_eval.candidates(include_large=False)]
        self.assertEqual(ids, ["Qwen/Qwen3-Embedding-0.6B", "BAAI/bge-m3", "sentence-transformers/all-MiniLM-L6-v2"])

    def test_include_large_adds_the_4b_with_a_big_memory_requirement(self):
        rows = rag_eval.candidates(include_large=True)
        self.assertEqual(rows[-1][0], "Qwen/Qwen3-Embedding-4B")
        self.assertGreaterEqual(rag_eval.REQUIRED_MB["Qwen/Qwen3-Embedding-4B"], 9000)

    def test_big_models_use_auto_dtype_and_minilm_stays_float32(self):
        dtypes = {c[0]: c[1] for c in rag_eval.candidates(True)}
        self.assertEqual(dtypes["sentence-transformers/all-MiniLM-L6-v2"], "float32")
        self.assertTrue(all(v == "auto" for k, v in dtypes.items() if "MiniLM" not in k))

    def test_auto_dtype_per_accelerator(self):
        import types
        def torch_stub(cuda=False, mps=False, bf16=True):
            return types.SimpleNamespace(
                cuda=types.SimpleNamespace(is_available=lambda: cuda, is_bf16_supported=lambda: bf16),
                backends=types.SimpleNamespace(mps=types.SimpleNamespace(is_available=lambda: mps)))
        self.assertEqual(rag_eval.auto_dtype(torch_stub(cuda=True)), "bfloat16")
        self.assertEqual(rag_eval.auto_dtype(torch_stub(cuda=True, bf16=False)), "float16")
        self.assertEqual(rag_eval.auto_dtype(torch_stub(mps=True)), "bfloat16")
        self.assertEqual(rag_eval.auto_dtype(torch_stub()), "float32")
        self.assertEqual(rag_eval.auto_dtype(None), "float32")
        self.assertEqual(rag_eval.resolve_dtype("float16", torch_stub(cuda=True)), "float16")
        self.assertEqual(rag_eval.resolve_dtype("auto", torch_stub(cuda=True)), "bfloat16")

    def test_qwen_embedding_models_get_the_instruction_prompt_others_do_not(self):
        for model in ("Qwen/Qwen3-Embedding-0.6B", "Qwen/Qwen3-Embedding-4B", "Qwen/Qwen3-Embedding-8B"):
            prompt = rag_eval.prompt_for(model)
            self.assertTrue(prompt.startswith("Instruct: "))
            self.assertTrue(prompt.endswith("\nQuery:"))               # exactly the model card's format
        self.assertEqual(rag_eval.prompt_for("BAAI/bge-m3"), "")
        self.assertEqual(rag_eval.prompt_for("sentence-transformers/all-MiniLM-L6-v2"), "")

    def test_system_summary_names_the_platform_and_memory(self):
        text = rag_eval.system_summary()
        for word in ("Python", "RAM"):
            self.assertIn(word, text)

    def test_report_file_header_includes_the_system_summary(self):
        row = rag_eval.CandidateResult(model="m", dtype="float16", dim=8, chunk_words=100, chunks=5,
                                       recall={1: 1.0, 4: 1.0, 10: 1.0}, mrr=1.0,
                                       ingest_chunks_per_s=1.0, peak_rss_mb=10.0)
        text = rag_eval.report_with_header([row], "Test machine")
        self.assertTrue(text.startswith("Machine: Test machine"))
        self.assertIn("| m |", text)


class ReportTests(unittest.TestCase):
    def _row(self, model):
        return rag_eval.CandidateResult(
            model=model, dtype="float16", dim=1024, chunk_words=320, chunks=900,
            recall={1: 0.5, 4: 0.8, 10: 0.9}, mrr=0.61, ingest_chunks_per_s=12.3, peak_rss_mb=1800.0,
        )

    def test_format_report_has_one_row_per_candidate(self):
        text = rag_eval.format_report([self._row("m/a"), self._row("m/b")])
        body = [l for l in text.splitlines() if l.startswith("| m/")]
        self.assertEqual(len(body), 2)
        self.assertIn("recall@4", text)
        self.assertIn("0.80", text)


class ScoringTests(unittest.TestCase):
    def test_rank_hits_uses_embedding_scores(self):
        import numpy as np
        chunks = ["red apple", "blue sea", "green leaf"]
        vecs = np.array([[1, 0], [0, 1], [0.6, 0.8]], dtype=np.float32)
        q = np.array([0, 1], dtype=np.float32)
        hits = rag_eval.rank_hits(q, vecs, chunks, ["blue sea"], k=2)
        self.assertEqual(hits, [True, False])

    def test_rank_hits_pads_when_fewer_chunks_than_k(self):
        import numpy as np
        hits = rag_eval.rank_hits(np.array([1, 0], dtype=np.float32),
                                  np.array([[1, 0]], dtype=np.float32), ["only"], ["only"], k=3)
        self.assertEqual(hits, [True, False, False])

    def test_evaluate_with_fake_embedders(self):
        import numpy as np
        docs = {"a.txt": "alpha beta gamma delta " * 3}
        qs = [rag_eval.Question("what?", "a.txt", ["alpha"])]

        def embed_docs(texts):
            return np.ones((len(texts), 2), dtype=np.float32)

        def embed_query(text):
            return np.ones(2, dtype=np.float32)

        result = rag_eval.evaluate(docs, qs, embed_docs, embed_query, chunk_words=4, overlap=0)
        self.assertEqual(result["chunks"], 3)
        self.assertEqual(result["ranked_hits"][0][0], True)
        self.assertEqual(result["dim"], 2)

    def test_evaluate_searches_whole_library_and_hit_must_come_from_expected_doc(self):
        import numpy as np
        # Same phrase in both docs: only the chunk from the question's own doc counts.
        docs = {"a.txt": "shared phrase here ok", "b.txt": "shared phrase here ok"}
        qs = [rag_eval.Question("q", "b.txt", ["shared phrase"])]
        result = rag_eval.evaluate(
            docs, qs, lambda t: np.ones((len(t), 2), dtype=np.float32),
            lambda t: np.ones(2, dtype=np.float32), chunk_words=4, overlap=0)
        # All scores tie; stable order puts a.txt's chunk first (a miss), b.txt's second (a hit).
        self.assertEqual(result["ranked_hits"][0][:2], [False, True])


class HybridScoringTests(unittest.TestCase):
    def _library(self):
        import numpy as np
        chunks = [f"filler text {i}" for i in range(30)] + ["the invoice INV-77341 is overdue"]
        vecs = np.zeros((31, 3), dtype=np.float32)
        for i in range(30):
            vecs[i] = [1.0, 0.01 * (i + 1), 0.0]
        vecs[30] = [0.0, 0.0, 1.0]
        vecs /= np.linalg.norm(vecs, axis=1, keepdims=True)
        return chunks, vecs, np.array([1.0, 0.0, 0.0], dtype=np.float32)

    def test_hybrid_rank_finds_exact_term(self):
        chunks, vecs, q = self._library()
        needles = ["INV-77341"]
        self.assertNotIn(True, rag_eval.rank_hits(q, vecs, chunks, needles, k=10))       # vector never sees it
        conn = rag_eval.build_fts(chunks)
        hits = rag_eval.rank_hits_hybrid("INV-77341", q, vecs, chunks, needles, 2, None, None, conn)
        self.assertEqual(hits, [False, True])            # tie with vector rank 1 goes to the higher cosine
        hits = rag_eval.rank_hits_hybrid("INV-77341", q, vecs, chunks, needles, 3, None, None, conn)
        self.assertEqual(hits, [False, True, False])

    def test_hybrid_rank_without_searchable_text_equals_vector(self):
        chunks, vecs, q = self._library()
        conn = rag_eval.build_fts(chunks)
        self.assertEqual(rag_eval.rank_hits_hybrid("--- ::", q, vecs, chunks, ["filler text 0"], 3, None, None, conn),
                         rag_eval.rank_hits(q, vecs, chunks, ["filler text 0"], 3))

    def test_hybrid_rank_respects_expected_document(self):
        chunks, vecs, q = self._library()
        conn = rag_eval.build_fts(chunks)
        docs = ["a.txt"] * 30 + ["b.txt"]
        self.assertEqual(rag_eval.rank_hits_hybrid("INV-77341", q, vecs, chunks, ["INV-77341"], 2, docs, "a.txt", conn),
                         [False, False])

    def test_evaluate_both_modes_from_one_embedding_pass(self):
        import numpy as np
        docs = {"a.txt": " ".join(f"w{i}" for i in range(40)) + " INV-77341 " + " ".join(f"v{i}" for i in range(40))}
        qs = [rag_eval.Question("INV-77341", "a.txt", ["INV-77341"])]
        calls = []

        def embed_docs(texts):
            calls.append(len(texts))
            return np.ones((len(texts), 2), dtype=np.float32)

        result = rag_eval.evaluate(docs, qs, embed_docs, lambda t: np.ones(2, dtype=np.float32),
                                   chunk_words=20, overlap=0, search="both")
        self.assertEqual(len(calls), 1)
        self.assertEqual(set(result["ranked_by_mode"]), {"vector", "hybrid"})
        self.assertEqual(result["ranked_hits"], result["ranked_by_mode"]["vector"])
        self.assertTrue(any(result["ranked_by_mode"]["hybrid"][0]))

    def test_report_has_a_search_column(self):
        row = rag_eval.CandidateResult(model="m/a", dtype="float16", dim=8, chunk_words=100, chunks=5,
                                       recall={1: 0.5, 4: 0.8, 10: 0.9}, mrr=0.6, ingest_chunks_per_s=1.0,
                                       peak_rss_mb=10.0, search="hybrid")
        text = rag_eval.format_report([row])
        self.assertIn("| search |", text)
        self.assertIn("| m/a | hybrid |", text)


if __name__ == "__main__":
    unittest.main()
