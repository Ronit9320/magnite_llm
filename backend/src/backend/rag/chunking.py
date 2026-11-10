from __future__ import annotations

import os
import re
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


def chunk_text(text: str, chunk_size: int = 512, chunk_overlap: int = 64) -> list[str]:
    """Split running text into overlapping token-sized chunks.

    'Tokens' here are spaces-split words — a deterministic approximation that
    needs no model download, so ingest never depends on the tokenizer. Chunks
    stay contiguous (each starts after ``chunk_size - chunk_overlap`` words).
    """
    if chunk_size < 1 or chunk_overlap < 0 or chunk_overlap >= chunk_size:
        raise ValueError("Require positive chunk size and 0 <= overlap < chunk size")
    tokens = text.split()
    if not tokens:
        return []
    step = max(1, chunk_size - chunk_overlap)
    chunks: list[str] = []
    i = 0
    while i < len(tokens):
        chunk = tokens[i : i + chunk_size]
        if not chunk:
            break
        chunks.append(" ".join(chunk))
        if len(chunk) < chunk_size:
            break
        i += step
    return chunks


def count_tokens(text: str) -> int:
    return len(text.split())
