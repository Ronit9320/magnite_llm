import re
import unittest

from backend.rag.chunking import TOKENS_PER_WORD, _weight, chunk_text, fit_chunking
from backend.rag.schemas import RagConfig, RagIngestRequest


def prose(sentences=200, words=12):
    return " ".join(
        " ".join(f"w{s}x{i}" for i in range(words - 1)) + f" end{s}." for s in range(sentences))


def legacy_chunks(text, size, overlap):
    """The previous fixed-window algorithm, used to pin the hard-split behaviour."""
    tokens = text.split()
    step = max(1, size - overlap)
    out, i = [], 0
    while i < len(tokens):
        window = tokens[i:i + size]
        out.append(" ".join(window))
        if len(window) < size:
            break
        i += step
    return out


def covers_every_word(text, chunks):
    seen = set(w for c in chunks for w in c.split())
    return all(w in seen for w in text.split())


class InvariantTests(unittest.TestCase):
    def check(self, text, size, overlap):
        chunks = chunk_text(text, size, overlap)
        self.assertTrue(chunks)
        for c in chunks:
            self.assertLessEqual(len(c.split()), size)
        self.assertTrue(covers_every_word(text, chunks))
        return chunks

    def test_prose(self):
        self.check(prose(), 50, 10)

    def test_ten_thousand_words_without_punctuation(self):
        text = " ".join(f"tok{i}" for i in range(10_000))
        chunks = self.check(text, 100, 20)
        self.assertEqual(chunks, legacy_chunks(text, 100, 20))        # hard split keeps the old stepping

    def test_code_block(self):
        code = "\n".join(f"    result_{i} = compute(value_{i}, offset)" for i in range(400))
        self.check(code, 64, 8)

    def test_spaceless_cjk_respects_the_size_limit(self):
        text = "".join(chr(0x4E00 + (i * 7) % 2000) for i in range(6000))      # no spaces at all
        chunks = chunk_text(text, 100, 10)
        self.assertGreater(len(chunks), 10)
        for c in chunks:
            self.assertLessEqual(len(c), 100 * 1.5 + 1)                          # ~1.5 CJK chars per word-equivalent
        joined = "".join(chunks)
        for ch in set(text):
            self.assertIn(ch, joined)

    def test_cjk_with_sentence_punctuation_splits_between_sentences(self):
        text = "".join(f"这是第{i}个句子，内容很长很长很长很长很长很长。 " for i in range(300))
        chunks = chunk_text(text, 64, 0)
        self.assertGreater(len(chunks), 5)
        for c in chunks:
            self.assertLessEqual(sum(_weight(t) for t in c.split()), 64)     # the size invariant, in weight
            self.assertTrue(c.endswith("。"))                               # cut between sentences

    def test_one_enormous_unbroken_token(self):
        text = "a" * 50_000
        chunks = chunk_text(text, 64, 8)
        self.assertGreater(len(chunks), 1)
        self.assertEqual(set("".join(chunks)), {"a"})
        for c in chunks:
            self.assertLessEqual(len(c), 64 * 6 + 1)

    def test_long_urls_do_not_blow_the_budget(self):
        text = " ".join("https://example.com/" + "segment/" * 20 + str(i) for i in range(200))
        for c in chunk_text(text, 64, 8):
            self.assertLessEqual(len(c), 64 * 6 + 30)

    def test_terminates_when_overlap_is_almost_the_size(self):
        chunks = self.check(prose(30), 10, 9)
        self.assertLess(len(chunks), 5000)


class BoundaryTests(unittest.TestCase):
    def test_prose_chunks_end_on_sentence_boundary(self):
        chunks = chunk_text(prose(), 50, 10)
        self.assertGreater(len(chunks), 5)
        for c in chunks:
            self.assertRegex(c, r"[.?!]$")

    def test_chunks_start_on_sentence_boundary(self):
        for c in chunk_text(prose(), 50, 10):
            self.assertRegex(c, r"^w\d+x0 ")

    def test_overlap_is_whole_sentences_within_budget(self):
        chunks = chunk_text(prose(80, 8), 40, 10)
        for a, b in zip(chunks, chunks[1:]):
            last_sentence = re.split(r"(?<=\.) ", a)[-1]
            self.assertTrue(b.startswith(last_sentence), (a[-60:], b[:60]))
            shared = 0
            for k in range(1, len(a.split()) + 1):
                if b.startswith(" ".join(a.split()[-k:])):
                    shared = k
            self.assertLessEqual(shared, 10)

    def test_no_overlap_when_a_sentence_is_longer_than_the_overlap(self):
        chunks = chunk_text(prose(60, 20), 45, 10)
        for a, b in zip(chunks, chunks[1:]):
            self.assertFalse(b.startswith(re.split(r"(?<=\.) ", a)[-1]))

    def test_list_lines_are_boundaries(self):
        text = "\n".join(f"- item number {i} in the list" for i in range(100))
        for c in chunk_text(text, 20, 0):
            self.assertTrue(c.startswith("- item"))
            self.assertTrue(re.search(r"list$", c))

    def test_wrapped_prose_is_not_split_at_every_line(self):
        wrapped = "The quick brown fox jumps over\nthe lazy dog and keeps running\nacross the wide field. Another sentence follows."
        chunks = chunk_text(wrapped, 12, 0)
        self.assertEqual(chunks[0], "The quick brown fox jumps over the lazy dog and keeps running across the wide field."[:len(chunks[0])])
        self.assertTrue(chunks[0].endswith("running") or chunks[0].endswith("field."))

    def test_abbreviation_like_text_does_not_crash(self):
        chunk_text("Dr. Smith went to Washington D.C. at 3.5 p.m. e.g. today. " * 50, 20, 4)

    def test_last_chunk_is_never_only_overlap(self):
        chunks = chunk_text(prose(40, 8), 40, 10)
        last_words = chunks[-1].split()
        before = " ".join(chunks[-2].split()[-10:])
        self.assertNotEqual(" ".join(last_words), before)


class EdgeTests(unittest.TestCase):
    def test_empty_and_whitespace_input_returns_empty_list(self):
        self.assertEqual(chunk_text("", 10, 2), [])
        self.assertEqual(chunk_text("  \n\t  ", 10, 2), [])

    def test_short_text_is_one_chunk(self):
        self.assertEqual(chunk_text("Just one short sentence.", 100, 10), ["Just one short sentence."])

    def test_invalid_arguments_raise_value_error(self):
        for size, overlap in ((0, 0), (-1, 0), (5, -1), (5, 5), (5, 9)):
            with self.assertRaises(ValueError):
                chunk_text("sample", size, overlap)

    def test_whitespace_is_normalized(self):
        self.assertEqual(chunk_text("one   two\t\tthree", 10, 0), ["one two three"])

    def test_default_arguments(self):
        self.assertEqual(len(chunk_text(" ".join(["w"] * 1000))), 3)      # 512 words, overlap 64


class FitChunkingTests(unittest.TestCase):
    def test_minilm_limit_clamps_the_default_size(self):
        self.assertEqual(fit_chunking(512, 64, 256), (149, 37))
        self.assertEqual(int((256 - 2) / TOKENS_PER_WORD), 149)

    def test_noop_when_it_fits(self):
        self.assertEqual(fit_chunking(320, 40, 8192), (320, 40))
        self.assertEqual(fit_chunking(100, 10, 256), (100, 10))

    def test_none_limit_returns_inputs_unchanged(self):
        self.assertEqual(fit_chunking(512, 64, None), (512, 64))

    def test_tiny_limit_still_gives_a_valid_pair(self):
        size, overlap = fit_chunking(512, 64, 8)
        self.assertGreaterEqual(size, 16)
        self.assertLess(overlap, size)

    def test_result_always_satisfies_chunk_text_rules(self):
        for limit in (8, 64, 128, 256, 512, 8192):
            size, overlap = fit_chunking(512, 64, limit)
            chunk_text("some words here", size, overlap)


class SchemaTests(unittest.TestCase):
    def test_schema_descriptions_say_words(self):
        for model in (RagConfig, RagIngestRequest):
            for name in ("chunk_size", "chunk_overlap"):
                description = model.model_fields[name].description or ""
                self.assertIn("words", description, (model.__name__, name))
                self.assertNotIn("tokens", description)

    def test_default_config_chunk_sizes(self):
        cfg = RagConfig()
        self.assertEqual((cfg.chunk_size, cfg.chunk_overlap), (320, 40))


if __name__ == "__main__":
    unittest.main()
