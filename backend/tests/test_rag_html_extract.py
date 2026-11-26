"""HTML text extraction is linear and keeps the old extractor's behaviour (break report 2026-11-20, F4).

Before: extraction ran on ``html.parser.HTMLParser``, which is quadratic on malformed input on Python
builds without the 2025 fixes (3.13.0 here): 50 KB of ``a<b`` text took 7 s, 100 KB of ``<!--`` 4.5 s, and
the call cannot be interrupted while it holds the single ingest lock.
"""
import random
import time
import unittest
from html.parser import HTMLParser

from backend.rag.chunking import extract_content


class OracleExtractor(HTMLParser):
    """The previous implementation, kept as the reference for well-formed documents."""

    _SKIPPED = ("script", "style")

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self._out, self._held, self._skip = [], [], None

    def handle_starttag(self, tag, attrs):
        if self._skip is None and tag in self._SKIPPED:
            self._skip, self._held = tag, []
        self._out.append(" ")

    def handle_endtag(self, tag):
        if self._skip == tag:
            self._skip, self._held = None, []
        self._out.append(" ")

    def handle_data(self, data):
        (self._held if self._skip else self._out).append(data)

    def handle_comment(self, data):
        self._out.append(" ")

    def handle_decl(self, decl):
        self._out.append(" ")

    def handle_pi(self, data):
        self._out.append(" ")

    def text(self):
        self.close()
        unclosed = (self._held + [self.rawdata]) if self._skip and self.rawdata else self._held if self._skip else []
        return "".join(self._out + unclosed)


def words(html):
    return extract_content(html, "html").split()


class LinearTimeTests(unittest.TestCase):
    """Each input class below made the stdlib parser super-linear; all must finish far inside a second."""

    def assertFast(self, text, limit=1.0):
        start = time.perf_counter()
        extract_content(text, "html")
        self.assertLess(time.perf_counter() - start, limit)

    def test_stray_tag_openers_in_prose(self):
        self.assertFast("x<y and a<b then c<d. " * 1400)

    def test_repeated_unclosed_tags(self):
        self.assertFast("<a " * 10_000)

    def test_repeated_unclosed_attribute(self):
        self.assertFast("<a b=" * 30_000)

    def test_repeated_unclosed_comments(self):
        self.assertFast("<!--" * 30_000)

    def test_repeated_end_tag_openers(self):
        self.assertFast("</" * 75_000)

    def test_repeated_processing_instructions(self):
        self.assertFast("<?" * 75_000)

    def test_repeated_cdata_openers(self):
        self.assertFast("<![CDATA[" * 20_000)

    def test_repeated_unmatched_quotes_inside_tags(self):
        self.assertFast("<a \"x' " * 20_000)
        self.assertFast("<a 'x\" " * 20_000)

    def test_many_quoted_attributes_before_a_far_tag_end(self):
        self.assertFast("<a " + 'k="v" ' * 100_000 + ">text")

    def test_large_ordinary_page(self):
        page = "<html><body>" + "<p class='c'>Some <b>bold</b> text &amp; more.</p><!-- c -->" * 40_000 + "</body></html>"
        self.assertFast(page, limit=3.0)

    def test_unclosed_script_with_many_openers(self):
        self.assertFast("<script>" + "if (a<b) { x = '<div>'; }" * 30_000)


class EdgeCaseTests(unittest.TestCase):
    """Outputs recorded from the previous extractor; the replacement must agree."""

    def test_cases(self):
        cases = {
            "before <!-- never closed after text": ["before", "<!--", "never", "closed", "after", "text"],
            "a <!-- c --> b": ["a", "b"],
            "<![CDATA[ x > y ]]> after": ["after"],
            "<?php echo 1; ?> after": ["after"],
            '<a title="x > y">link</a>': ["link"],
            "text <img alt='a>b'> more": ["text", "more"],
            "<SCRIPT>var a</SCRIPT>ok<STYLE>p{}</STYLE>done": ["ok", "done"],
            "<script>if (a</b) {}</script>tail": ["tail"],
            "<p>a<br/>b<br>c</p>": ["a", "b", "c"],
            "x &amp; &lt;b&gt; &nbsp;z &#65; &#x41; &copy; &bogus; &": ["x", "&", "<b>", "z", "A", "A", "©", "&bogus;", "&"],
            "a < b and 1<2": ["a", "<", "b", "and", "1<2"],
            "<div class=a>text": ["text"],
            "a <b": ["a", "<b"],
            "<<p>>x": ["<", ">x"],
            "<!DOCTYPE html><html><body>hi</body></html>": ["hi"],
            "</>after": ["after"],
            "</ p>x": ["x"],
            "<a href='x'>y</a": ["y</a"],
            "<textarea>keep <b>this</b></textarea>": ["keep", "this"],
            "<title>T</title><p>body</p>": ["T", "body"],
            "<p>a</p\n>b": ["a", "b"],
        }
        for source, expected in cases.items():
            with self.subTest(source=source):
                self.assertEqual(words(source), expected)

    def test_unclosed_script_keeps_the_rest_of_the_document(self):
        self.assertEqual(words("<p>before</p><script>var a=1; after text"), ["before", "var", "a=1;", "after", "text"])


class MatchesTheOldExtractorOnWellFormedHtml(unittest.TestCase):
    # No <textarea> with markup inside: from Python 3.12.12 html.parser keeps textarea content as
    # literal text, so the oracle would depend on the patch release. the explicit cases above cover it.
    FRAGMENTS = ["<p>", "</p>", "<div class='x y'>", "</div>", "<b>", "</b>", "<a href=\"u?a=1&b=2\">", "</a>", "<br/>", "<br>",
                 "<img alt='a>b' src=x>", "<!-- note -->", "<!DOCTYPE html>", "<?xml version='1.0'?>",
                 "<script>var s = '<b>x</b>'; if (a < b) {}</script>", "<style>p > a { }</style>", "<SCRIPT>x</SCRIPT>",
                 "plain words here", "Second sentence.", "&amp;", "&lt;tag&gt;", "&#169;", "&nbsp;", "&#x1F600;", "café", "日本語",
                 "<li>", "<td colspan=2>", "<h1>", "</h1>", "<title>T</title>"]

    def test_random_documents(self):
        rng = random.Random(2026)
        for n in range(500):
            html = "".join(rng.choice(self.FRAGMENTS) + rng.choice(["", " ", "\n"]) for _ in range(rng.randint(1, 60)))
            oracle = OracleExtractor()
            oracle.feed(html)
            with self.subTest(n=n, html=html[:120]):
                self.assertEqual(words(html), oracle.text().split())


if __name__ == "__main__":
    unittest.main()
