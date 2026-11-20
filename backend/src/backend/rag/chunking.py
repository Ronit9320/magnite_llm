from __future__ import annotations

import codecs
import math
import os
import re
from typing import Optional
from html import unescape

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
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as exc:
        raise ValueError(f"Cannot read file: {path}") from exc
    try:
        return decode_text_bytes(data)
    except ValueError as exc:
        raise ValueError(f"{os.path.basename(path)}: {exc}") from None


_BINARY = ("This looks like a binary file, not text. Supported types are PDF, TXT, MD and HTML; "
           "export other formats (DOCX, images, ...) to PDF or plain text first.")


def decode_text_bytes(data: bytes) -> str:
    """Decode a text file: UTF-16 (with a byte order mark), UTF-8 (with or without one), else legacy
    8-bit text. Raises ValueError for anything that is not text (NUL bytes, mostly control characters)."""
    if data.startswith((codecs.BOM_UTF16_LE, codecs.BOM_UTF16_BE)):
        try:
            return data.decode("utf-16")
        except UnicodeDecodeError as exc:
            raise ValueError("Cannot decode this UTF-16 text file") from exc
    if b"\x00" in data:
        raise ValueError(_BINARY)
    try:
        text = data.decode("utf-8-sig")
    except UnicodeDecodeError:
        text = data.decode("latin-1")
    sample = text[:20_000]
    control = sum(1 for ch in sample if (ch < " " and ch not in "\t\n\r\f") or "\x7f" <= ch <= "\x9f")
    if control > 0.02 * max(1, len(sample)):
        raise ValueError(_BINARY)
    return text


# ---- HTML text extraction -----------------------------------------------------------------
#
# A hand-written single pass instead of ``html.parser.HTMLParser``: on Python builds without the 2025
# fixes that parser is quadratic on malformed input (50 KB of "a<b" text took 7 s), cannot be interrupted,
# and runs while the single ingest lock is held. Every ``find`` below starts where the previous construct
# ended, and an unterminated construct hands the rest of the document over as text, so the work is O(n).

_TAG_NAME = re.compile(r"[A-Za-z][^\s/>]*")
_QUOTE = re.compile("[\"']")
_RAW_TEXT_END = {name: re.compile(r"</%s" % name, re.IGNORECASE) for name in ("script", "style")}


def _tag_end(text: str, pos: int) -> int:
    """Index of the ``>`` that ends a tag whose name ended at ``pos``. ``>`` inside quoted attribute
    values does not count; a quote that is never closed is ignored. -1 when there is no ``>``."""
    gt = text.find(">", pos)
    while gt >= 0:
        quote = _QUOTE.search(text, pos, gt)
        if quote is None:
            return gt
        close = text.find(quote.group(), quote.end())
        if close < 0:
            return gt
        pos = close + 1
        if close > gt:
            gt = text.find(">", pos)
    return -1


def _strip_html(text: str) -> str:
    """Visible text of an HTML document in one linear pass.

    Every tag boundary becomes a space, <script>/<style> bodies are dropped, comments, doctypes,
    processing instructions and CDATA are dropped, and character references are unescaped. Anything
    unterminated (a tag, a comment, a script) keeps its text: for retrieval, losing the rest of a
    document is worse than keeping some markup.
    """
    out: list[str] = []
    pos, n = 0, len(text)
    while pos < n:
        lt = text.find("<", pos)
        if lt < 0:
            out.append(unescape(text[pos:]))
            break
        if lt > pos:
            out.append(unescape(text[pos:lt]))
        nxt = text[lt + 1:lt + 2]
        end = -1                                   # index of the last character of this construct
        kind = ""
        if nxt == "!":
            if text.startswith("<!--", lt):
                close = text.find("-->", lt + 4)
                end = close + 2 if close >= 0 else -1
            elif text.startswith("<![CDATA[", lt):
                close = text.find("]]>", lt + 9)
                end = close + 2 if close >= 0 else -1
            else:
                end = text.find(">", lt + 2)
        elif nxt == "?":
            end = text.find(">", lt + 2)
        elif nxt == "/":
            if _TAG_NAME.match(text, lt + 2):
                end = _tag_end(text, lt + 2)
            else:
                end = text.find(">", lt + 2)
        else:
            match = _TAG_NAME.match(text, lt + 1)
            if match:
                kind = match.group().lower()
                end = _tag_end(text, match.end())
            else:                                  # "a < b", "1<2": not markup
                out.append("<")
                pos = lt + 1
                continue
        if end < 0:                                # nothing closes it: keep the rest as text
            out.append(unescape(text[lt:]))
            break
        out.append(" ")
        pos = end + 1
        raw_end = _RAW_TEXT_END.get(kind)
        if raw_end is not None and text[end - 1:end] != "/":
            closer = raw_end.search(text, pos)
            gt = text.find(">", closer.end()) if closer else -1
            if gt < 0:                             # an unclosed script/style keeps its text
                out.append(text[pos:])
                break
            out.append(" ")
            pos = gt + 1
    return "".join(out)


_FRONT_MATTER = re.compile(r"\A---[ \t]*\n(?:[ \t]*[\w.-]+[ \t]*:.*\n|[ \t]+.*\n|[ \t]*-[ \t].*\n|[ \t]*\n)*?---[ \t]*(?:\n|\Z)")
# Only real HTML element names: generics such as List<String> and comparisons are code, which is kept.
_HTML_TAG = re.compile(r"</?(?:b|i|u|s|em|strong|br|hr|p|div|span|a|img|ul|ol|li|table|thead|tbody|tr|td|th|h[1-6]|pre|sup|sub|"
                       r"details|summary|blockquote|center|font|del|ins|kbd|mark|small|big|figure|figcaption|section|article|"
                       r"nav|header|footer)\b[^<>\n]{0,300}>", re.IGNORECASE)
_TABLE_SEPARATOR = re.compile(r"^[ \t]*\|?[ \t]*:?-+:?[ \t]*(?:\|[ \t]*:?-+:?[ \t]*)+\|?[ \t]*$", re.MULTILINE)
_TABLE_ROW = re.compile(r"^[ \t]*\|(.*)\|[ \t]*$", re.MULTILINE)


def _strip_markdown(text: str) -> str:
    # Preserve code: technical documents often contain their answers there.
    text = _FRONT_MATTER.sub("", text, count=1)
    text = re.sub(r"^```[^\n]*$", "", text, flags=re.MULTILINE)
    text = text.replace("`", "")
    text = re.sub(r"^\s{0,3}#{1,6}\s+", "", text, flags=re.MULTILINE)
    text = re.sub(r"^ {0,3}\[[^\]\n]+\]:[ \t]+\S.*$", "", text, flags=re.MULTILINE)        # [1]: https://...
    text = re.sub(r"!\[([^\]]*)\]\([^)]*\)", r"\1", text)                                  # images before links
    text = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", text)
    text = re.sub(r"(?<![\w\]\)])\[([^\]\n]+)\]\[[^\]\n]*\]", r"\1", text)             # [text][ref], not a[i][j]
    text = _HTML_TAG.sub(" ", text)
    text = _TABLE_SEPARATOR.sub("", text)
    text = _TABLE_ROW.sub(lambda m: m.group(1).replace("|", " "), text)
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


def chunk_weight(text: str) -> int:
    """The weight (the unit of chunk sizes) of a chunk of text: the sum of its whitespace-separated tokens'."""
    return sum(_weight(token) for token in text.split())


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
        chunk = render(items[i:j])
        while j > i + 1 and chunk_weight(chunk) > size:    # pieces glued back into one token can weigh more than their sum
            j -= 1
            chunk = render(items[i:j])
        chunks.append(chunk)
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


def chunk_limit(max_seq_tokens: Optional[int]) -> Optional[int]:
    """The largest chunk size (words) that fits the model's token limit; None when unknown."""
    if not max_seq_tokens:
        return None
    return max(16, int((max_seq_tokens - 2) / TOKENS_PER_WORD))


def fit_chunking(chunk_size: int, chunk_overlap: int, max_seq_tokens: Optional[int]) -> tuple:
    """Clamp a chunk size (words) so a chunk fits the embedding model's token limit."""
    limit = chunk_limit(max_seq_tokens)
    if limit is None:
        return chunk_size, chunk_overlap
    if chunk_size <= limit:
        return chunk_size, chunk_overlap
    return limit, min(chunk_overlap, limit // 4)


def count_tokens(text: str) -> int:
    return len(text.split())
