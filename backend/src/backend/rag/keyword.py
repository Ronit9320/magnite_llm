"""Keyword-search helpers: safe FTS5 queries and Reciprocal Rank Fusion. Pure functions, no state."""
from __future__ import annotations

import sqlite3
from typing import Optional, Sequence

FTS_TABLE = "rag_chunks_fts"
RRF_K = 60
MAX_TERMS = 32


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
    Pieces without a letter or digit and repeated pieces (ignoring case) are dropped; at most
    ``MAX_TERMS`` pieces are kept. Returns None when nothing searchable is left.
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
        if len(pieces) >= MAX_TERMS:
            break
    return " OR ".join(pieces) if pieces else None


def rrf_fuse(rankings: Sequence[Sequence[int]], k: int = RRF_K) -> dict[int, float]:
    """Reciprocal Rank Fusion: each ranking (best first) adds 1/(k+rank), rank starting at 1."""
    fused: dict[int, float] = {}
    for ranking in rankings:
        for rank, item in enumerate(ranking, 1):
            fused[item] = fused.get(item, 0.0) + 1.0 / (k + rank)
    return fused
