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


if __name__ == "__main__":
    unittest.main()
