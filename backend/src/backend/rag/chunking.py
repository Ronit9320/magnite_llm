from __future__ import annotations

import math
import os
import re
from typing import Optional
from html.parser import HTMLParser

SUPPORTED_EXTS = ("txt", "md", "html", "pdf")


def detect_kind(name: str, kind: str = "") -> str:
    """Infer a document kind from an explicit value, else a filename/ext."""
    kind = (kind or "").lower().lstrip(".")
    if kind in SUPPORTED_EXTS:
        return kind
    ext = os.path.splitext(name)[1].lower().lstrip(".")
    return ext if ext in SUPPORTED_EXTS else "txt"


def extract_text(path: str, kind: str) -> str:
    """Extract plain text from a supported document file."""
    if kind == "pdf":
        return _extract_pdf(path)
    return extract_content(_read_text(path), kind)


def extract_content(text: str, kind: str) -> str:
    if kind == "pdf":
        raise ValueError("Raw PDF content is unsupported; provide a PDF file path")
    if kind == "html":
        return _strip_html(text)
    if kind == "md":
        return _strip_markdown(text)
    return text


def _read_text(path: str) -> str:
    for enc in ("utf-8-sig", "latin-1"):
        try:
            with open(path, "r", encoding=enc) as f:
                return f.read()
        except (UnicodeDecodeError, OSError):
            continue
    raise ValueError(f"Cannot decode text file: {path}")


class _TextExtractor(HTMLParser):
    """Visible text of an HTML document in one linear pass.

    Every tag boundary becomes a space, <script>/<style> bodies are dropped, and character
    references are unescaped. A script or style that is never closed keeps its text (a browser
    would swallow the rest of the page; for retrieval, losing the document is worse).
    """

    _SKIPPED = ("script", "style")

    def __init__(self):
        super().__init__(convert_charrefs=True)
        self._out: list[str] = []
        self._held: list[str] = []
        self._skip: str | None = None

    def handle_starttag(self, tag, attrs):
        if self._skip is None and tag in self._SKIPPED:
            self._skip = tag
            self._held = []
        self._out.append(" ")

    def handle_endtag(self, tag):
        if self._skip == tag:
            self._skip = None
            self._held = []
        self._out.append(" ")

    def handle_data(self, data):
        (self._held if self._skip else self._out).append(data)

    def handle_comment(self, data):
        self._out.append(" ")

    def handle_decl(self, decl):
        self._out.append(" ")

    def handle_pi(self, data):
        self._out.append(" ")

    def text(self) -> str:
        self.close()
        # Some Python releases hand the content of an unclosed <script>/<style> to handle_data at
        # close(); others leave it unparsed in `rawdata`. Take whichever happened, never both.
        unclosed = (self._held + [self.rawdata]) if self._skip and self.rawdata else self._held if self._skip else []
        return "".join(self._out + unclosed)


def _strip_html(text: str) -> str:
    extractor = _TextExtractor()
    extractor.feed(text)
    return extractor.text()


def _strip_markdown(text: str) -> str:
    # Preserve code: technical documents often contain their answers there.
    text = re.sub(r"^```[^\n]*$", "", text, flags=re.MULTILINE)
    text = text.replace("`", "")
    text = re.sub(r"^\s{0,3}#{1,6}\s+", "", text, flags=re.MULTILINE)
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"!\[([^\]]*)\]\([^)]*\)", r"\1", text)
    return text


def _extract_pdf(path: str) -> str:
    try:
        from pypdf import PdfReader
    except ImportError:
        raise ValueError(
            "PDF extraction needs 'pypdf'. Install with "
            "'uv pip install --python .venv/bin/python pypdf'."
        )
    try:
        reader = PdfReader(path)
        if reader.is_encrypted:
            raise ValueError("Encrypted PDFs are unsupported; provide an unencrypted copy")
        pages = [page.extract_text() or "" for page in reader.pages]
    except Exception as exc:
        raise ValueError(f"Cannot extract PDF (corrupt, encrypted, or unsupported): {exc}") from exc
    if not any(pages):
        raise ValueError(f"No text extracted from PDF: {path}")
    return "\n\n".join(pages)


# ---- chunking -----------------------------------------------------------------------------
#
# Sizes are "words": whitespace-separated tokens, so ingest never needs a tokenizer. Unspaced
# scripts (CJK) and very long tokens (URLs, hashes) count as several words so a chunk still
# stays near its size limit in real tokens.

TOKENS_PER_WORD = 1.7        # measured 1.3-1.65 on real text; deliberately conservative
_CJK = re.compile("[\u3040-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uac00-\ud7af\uf900-\ufaff]")
_CJK_CHARS_PER_WORD = 1.5
_LONG_TOKEN_CHARS = 24
_LONG_TOKEN_CHARS_PER_WORD = 6
_PARAGRAPH = re.compile(r"\n\s*\n")
# A sentence ends after . ! ? (plus closing quotes/brackets) before whitespace, or at a newline
# whose next line does not start lowercase (list items, code, table rows; wrapped prose continues).
_BOUNDARY = re.compile(r"([.!?][\"')\]]*)\s+|\n+(?=[^\sa-z])")


def _weight(token: str) -> int:
    n = len(token)
    if n <= _LONG_TOKEN_CHARS and token.isascii():
        return 1
    cjk = len(_CJK.findall(token))
    other = n - cjk
    weight = math.ceil(cjk / _CJK_CHARS_PER_WORD)
    if other:
        weight += math.ceil(other / _LONG_TOKEN_CHARS_PER_WORD) if other > _LONG_TOKEN_CHARS else 1
    return max(1, weight)


def _sentences(text: str):
    for paragraph in _PARAGRAPH.split(text):
        pos = 0
        for m in _BOUNDARY.finditer(paragraph):
            cut = m.end(1) if m.group(1) is not None else m.start()
            if cut > pos:
                yield paragraph[pos:cut]
            pos = m.end()
        if pos < len(paragraph):
            yield paragraph[pos:]


def _split_token(token: str, size: int) -> list:
    """Cut one oversized token into pieces whose weight is at most ``size``."""
    cjk_heavy = len(_CJK.findall(token)) * 2 > len(token)
    step = max(1, int(size * (_CJK_CHARS_PER_WORD if cjk_heavy else _LONG_TOKEN_CHARS_PER_WORD)))
    pieces, i = [], 0
    while i < len(token):
        piece = token[i:i + step]
        while _weight(piece) > size and len(piece) > 1:
            piece = piece[:max(1, int(len(piece) * 0.9))]
        pieces.append(piece)
        i += len(piece)
    return pieces


def _hard_split(tokens: list, size: int, overlap: int) -> list:
    """Fixed windows over one sentence that is longer than ``size`` (the old stepping, by weight)."""
    items = []                                   # (text, weight, glued to the previous piece)
    for token in tokens:
        weight = _weight(token)
        if weight <= size:
            items.append((token, weight, False))
        else:
            for n, piece in enumerate(_split_token(token, size)):
                items.append((piece, _weight(piece), n > 0))

    def render(part):
        out = []
        for text, _weight_, glued in part:
            out.append(text if glued or not out else " " + text)
        return "".join(out)

    chunks, i, n = [], 0, len(items)
    while i < n:
        j, total = i, 0
        while j < n and total + items[j][1] <= size:
            total += items[j][1]
            j += 1
        j = max(j, i + 1)
        chunks.append(render(items[i:j]))
        if j >= n:
            break
        k, shared = j, 0
        while k > i + 1 and shared + items[k - 1][1] <= overlap:
            k -= 1
            shared += items[k][1]
        i = max(k, i + 1)
    return chunks


def chunk_text(text: str, chunk_size: int = 512, chunk_overlap: int = 64) -> list[str]:
    """Split text into chunks of at most ``chunk_size`` words that start and end on sentence boundaries.

    Sentences are packed greedily; the next chunk begins with the trailing whole sentences that total
    at most ``chunk_overlap`` words. A single sentence longer than ``chunk_size`` is cut into fixed
    windows (step ``chunk_size - chunk_overlap``). Whitespace is normalized to single spaces.
    """
    if chunk_size < 1 or chunk_overlap < 0 or chunk_overlap >= chunk_size:
        raise ValueError("Require positive chunk size and 0 <= overlap < chunk size")
    chunks: list[str] = []
    current: list = []                           # (sentence text, weight)
    current_weight = 0
    fresh = 0                                    # sentences added since the last chunk was emitted
    for sentence in _sentences(text):
        tokens = sentence.split()
        if not tokens:
            continue
        if sentence.isascii() and len(sentence) <= _LONG_TOKEN_CHARS * len(tokens) and max(map(len, tokens)) <= _LONG_TOKEN_CHARS:
            weight = len(tokens)
        else:
            weight = sum(_weight(t) for t in tokens)
        if weight > chunk_size:
            if fresh:
                chunks.append(" ".join(t for t, _ in current))
            current, current_weight, fresh = [], 0, 0
            chunks.extend(_hard_split(tokens, chunk_size, chunk_overlap))
            continue
        if current_weight + weight > chunk_size:
            if fresh:
                chunks.append(" ".join(t for t, _ in current))
            carry, carried = [], 0
            for unit in reversed(current):
                if carried + unit[1] > chunk_overlap:
                    break
                carry.append(unit)
                carried += unit[1]
            carry.reverse()
            while carry and carried + weight > chunk_size:
                carried -= carry.pop(0)[1]
            current, current_weight, fresh = carry, carried, 0
        current.append((" ".join(tokens), weight))
        current_weight += weight
        fresh += 1
    if fresh:
        chunks.append(" ".join(t for t, _ in current))
    return chunks


def fit_chunking(chunk_size: int, chunk_overlap: int, max_seq_tokens: Optional[int]) -> tuple:
    """Clamp a chunk size (words) so a chunk fits the embedding model's token limit."""
    if not max_seq_tokens:
        return chunk_size, chunk_overlap
    limit = max(16, int((max_seq_tokens - 2) / TOKENS_PER_WORD))
    if chunk_size <= limit:
        return chunk_size, chunk_overlap
    return limit, min(chunk_overlap, limit // 4)


def count_tokens(text: str) -> int:
    return len(text.split())
