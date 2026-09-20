from __future__ import annotations

import html as _html
import os
import re

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
    text = _read_text(path)
    if kind == "html":
        return _strip_html(text)
    if kind == "md":
        return _strip_markdown(text)
    return text


def _read_text(path: str) -> str:
    for enc in ("utf-8", "utf-8-sig", "latin-1"):
        try:
            with open(path, "r", encoding=enc) as f:
                return f.read()
        except (UnicodeDecodeError, OSError):
            continue
    raise ValueError(f"Cannot decode text file: {path}")


_HANDLER_TAG = re.compile(r"<(script|style)[^>]*>.*?</\1>", re.IGNORECASE | re.DOTALL)
_TAG = re.compile(r"<[^>]+>")


def _strip_html(text: str) -> str:
    text = _HANDLER_TAG.sub(" ", text)
    text = _TAG.sub(" ", text)
    text = _html.unescape(text)
    return text


def _strip_markdown(text: str) -> str:
    # Remove fenced code blocks entirely (noise for embedding), keep the rest.
    text = re.sub(r"```.*?```", " ", text, flags=re.DOTALL)
    text = re.sub(r"`[^`]*`", " ", text)
    text = re.sub(r"[#>*_~\-]", " ", text)
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
    reader = PdfReader(path)
    pages = [page.extract_text() or "" for page in reader.pages]
    if not any(pages):
        raise ValueError(f"No text extracted from PDF: {path}")
    return "\n\n".join(pages)


def chunk_text(text: str, chunk_size: int = 512, chunk_overlap: int = 64) -> list[str]:
    """Split running text into overlapping token-sized chunks.

    'Tokens' here are spaces-split words — a deterministic approximation that
    needs no model download, so ingest never depends on the tokenizer. Chunks
    stay contiguous (each starts after ``chunk_size - chunk_overlap`` words).
    """
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