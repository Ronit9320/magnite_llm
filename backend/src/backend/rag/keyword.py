"""Keyword-search helpers: safe FTS5 queries and Reciprocal Rank Fusion. Pure functions, no state."""
from __future__ import annotations

import re
import sqlite3
from typing import Optional, Sequence

FTS_TABLE = "rag_chunks_fts"
RRF_K = 60
MAX_TERMS = 32
MAX_CJK_TERMS = 16
# Han, kana and hangul: scripts written without spaces, which the FTS5 tokenizer sees as one long token.
_CJK_RUN = re.compile("[\u3040-\u30ff\u3400-\u4dbf\u4e00-\u9fff\uac00-\ud7af\uf900-\ufaff]+")


def fts5_available(conn: sqlite3.Connection) -> bool:
    """True when this SQLite build can create FTS5 tables (probed with a TEMP table, then dropped)."""
    try:
        conn.execute("CREATE VIRTUAL TABLE temp._magnite_fts_probe USING fts5(x)")
    except sqlite3.Error:
        return False
    try:
        conn.execute("DROP TABLE temp._magnite_fts_probe")
    except sqlite3.Error:
        pass
    return True


def match_expression(text: str) -> Optional[str]:
    """Turn free text into an FTS5 MATCH expression in which nothing is an operator.

    The text is split on whitespace; each piece becomes a quoted FTS5 string (inner quotes doubled)
    and the pieces are OR-ed, so AND/OR/NEAR, ``:``, ``-``, ``*``, ``^`` and parentheses are literal.
    Pieces without a letter or digit and repeated pieces (ignoring case) are dropped. Past
    ``MAX_TERMS`` pieces the first half and the last half are kept: a long message states its topic
    at the start and its question at the end. Returns None when nothing searchable is left.
    """
    pieces: list[str] = []
    seen: set[str] = set()
    for piece in (text or "").split():
        if not any(ch.isalnum() for ch in piece):
            continue
        key = piece.casefold()
        if key in seen:
            continue
        seen.add(key)
        pieces.append('"' + piece.replace('"', '""') + '"')
    if len(pieces) > MAX_TERMS:
        head = MAX_TERMS // 2
        pieces = pieces[:head] + pieces[-(MAX_TERMS - head):]
    return " OR ".join(pieces) if pieces else None


_LITERAL_SYMBOLS = set("+#@_/\\")
MAX_LITERALS = 8


def literal_pieces(text: str) -> list[str]:
    """Identifier-like pieces of a query that should match verbatim: ``C++``, ``C#``, ``.NET``,
    ``user@example.com``, ``E_CONN_RESET``, ``INV-2024-0031``, ``v2.0.1``, ``192.168.0.1``. A piece
    qualifies when it has one of ``+ # @ _ / \\``, or a digit together with another symbol, or starts with a
    dot before a letter. Plain hyphenated words, contractions and abbreviations (``e.g.``) do not."""
    pieces: list[str] = []
    for raw in (text or "").split():
        piece = raw.lstrip("([{\"'").rstrip(".,;:!?)]}\"'")
        if len(piece) < 2 or len(piece) > 64 or piece in pieces:
            continue
        symbols = [ch for ch in piece if not ch.isalnum()]
        if not symbols:
            continue
        if (any(ch in _LITERAL_SYMBOLS for ch in piece) or (any(ch.isdigit() for ch in piece))
                or (piece[0] == "." and piece[1].isalpha())):
            pieces.append(piece)
        if len(pieces) >= MAX_LITERALS:
            break
    return pieces


def cjk_share(text: str) -> float:
    """Fraction of the non-space characters that are Han, kana or hangul."""
    chars = "".join((text or "").split())
    return sum(len(run) for run in _CJK_RUN.findall(chars)) / len(chars) if chars else 0.0


def cjk_terms(text: str) -> list[str]:
    """Search terms for unspaced scripts: the overlapping character pairs of every Han/kana/hangul run
    (a lone character for a run of one), without repeats, at most ``MAX_CJK_TERMS`` (first and last
    half when there are more). A chunk containing all the pairs contains the phrase."""
    terms: list[str] = []
    seen: set[str] = set()
    for run in _CJK_RUN.findall(text or ""):
        for term in ([run] if len(run) == 1 else [run[i:i + 2] for i in range(len(run) - 1)]):
            if term not in seen:
                seen.add(term)
                terms.append(term)
    if len(terms) > MAX_CJK_TERMS:
        head = MAX_CJK_TERMS // 2
        terms = terms[:head] + terms[-(MAX_CJK_TERMS - head):]
    return terms


def rrf_fuse(rankings: Sequence[Sequence[int]], k: int = RRF_K) -> dict[int, float]:
    """Reciprocal Rank Fusion: each ranking (best first) adds 1/(k+rank), rank starting at 1."""
    fused: dict[int, float] = {}
    for ranking in rankings:
        for rank, item in enumerate(ranking, 1):
            fused[item] = fused.get(item, 0.0) + 1.0 / (k + rank)
    return fused
