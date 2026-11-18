import sqlite3
import unittest

from backend.rag import keyword


def fts_table():
    conn = sqlite3.connect(":memory:")
    conn.execute("CREATE VIRTUAL TABLE t USING fts5(text, tokenize='unicode61 remove_diacritics 2')")
    return conn


class MatchExpressionTests(unittest.TestCase):
    def test_operators_are_literal(self):
        self.assertIn('"AND"', keyword.match_expression("a AND b"))
        expr = keyword.match_expression('x" OR "y')
        self.assertIn('"x"""', expr)
        self.assertIn('"""y"', expr)

    def test_pieces_joined_with_or(self):
        self.assertEqual(keyword.match_expression("alpha beta"), '"alpha" OR "beta"')

    def test_empty_and_punctuation_only_give_none(self):
        for text in ("", "   ", '"" -- :: ***'):
            self.assertIsNone(keyword.match_expression(text), text)

    def test_repeats_dropped_ignoring_case(self):
        self.assertEqual(keyword.match_expression("Foo foo FOO bar"), '"Foo" OR "bar"')

    def test_term_cap(self):
        expr = keyword.match_expression(" ".join(f"w{i}" for i in range(100)))
        self.assertEqual(len(expr.split(" OR ")), keyword.MAX_TERMS)

    def test_nasty_inputs_execute(self):
        conn = fts_table()
        conn.execute("INSERT INTO t(text) VALUES ('error ERR-404 occurred')")
        for text in ['"', "NEAR(a b)", "col:val", "ERR-404", "a*", "^start", "(x", "AND", "OR OR", "it's",
                     "MAGNITE_RAG_INDEX_MB", "日本語のテスト", "ünïcödé"]:
            expr = keyword.match_expression(text)
            if expr is not None:
                conn.execute("SELECT rowid FROM t WHERE t MATCH ?", (expr,)).fetchall()

    def test_hyphenated_id_matches_phrase(self):
        conn = fts_table()
        conn.execute("INSERT INTO t(rowid, text) VALUES (1, 'error ERR-404 occurred')")
        conn.execute("INSERT INTO t(rowid, text) VALUES (2, 'a 404 ERR happened')")
        rows = conn.execute("SELECT rowid FROM t WHERE t MATCH ?", (keyword.match_expression("ERR-404"),)).fetchall()
        self.assertEqual([r[0] for r in rows], [1])


class FusionTests(unittest.TestCase):
    def test_rrf_values(self):
        fused = keyword.rrf_fuse([[7, 8], [8, 9]])
        self.assertAlmostEqual(fused[8], 1 / 62 + 1 / 61)
        self.assertAlmostEqual(fused[7], 1 / 61)
        self.assertAlmostEqual(fused[9], 1 / 62)

    def test_constants(self):
        self.assertEqual(keyword.RRF_K, 60)
        self.assertEqual(keyword.FTS_TABLE, "rag_chunks_fts")


class AvailabilityTests(unittest.TestCase):
    def test_fts5_available_true_here(self):
        conn = sqlite3.connect(":memory:")
        self.assertTrue(keyword.fts5_available(conn))

    def test_probe_leaves_no_table_behind(self):
        conn = sqlite3.connect(":memory:")
        keyword.fts5_available(conn)
        self.assertEqual(conn.execute("SELECT count(*) FROM sqlite_temp_master").fetchone()[0], 0)


if __name__ == "__main__":
    unittest.main()
