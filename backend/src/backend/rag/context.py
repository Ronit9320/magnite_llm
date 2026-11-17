from __future__ import annotations

import re
from typing import Callable, Optional

from backend.rag.schemas import RagChunk

from backend.rag.embeddings import embed_query, is_available
from backend.rag.store import get_rag_store

NO_ROOM = "No room for retrieved context. Start a new chat or reduce max tokens."

_HEADER = (
    "The user has enabled retrieval over their local documents. Prefer "
    "these retrieved passages when answering:"
)
_SENTENCE_END = re.compile(r"[.!?][\"')\]]*(?=\s|$)")


def retrieve_chunks(query: str, top_k: Optional[int], min_score: Optional[float],
                    document_ids: Optional[list[str]]) -> list[RagChunk]:
    """Retrieve passages, reporting failures instead of silently answering without RAG."""
    store = get_rag_store()
    if store is None or not is_available():
        raise RuntimeError("RAG is unavailable. Install the ML extras or disable document retrieval.")
    if store.count_chunks() == 0:
        raise ValueError("No RAG documents are indexed. Add documents or disable document retrieval.")

    cfg = store.get_config()
    top_k = top_k if top_k is not None else cfg.top_k
    min_score = min_score if min_score is not None else cfg.min_score
    query_vec = embed_query(query, cfg.model, cfg.truncate_dim)
    chunks = store.search(query_vec, cfg.model, top_k=top_k, min_score=min_score, document_ids=document_ids)
    if not chunks:
        raise ValueError("No matching document passages. Lower the similarity threshold or disable retrieval.")
    return chunks


def _prefix(number: int, chunk: RagChunk) -> str:
    return f"\n\n[{number}] (from {chunk.document_name}) "


def _trim_to_fit(base: str, text: str, fits: Callable[[str], bool]) -> str:
    """Longest prefix of ``text`` that fits after ``base``, cut at a sentence end, else a word end."""
    low, high = 0, len(text)
    while low < high:
        mid = (low + high + 1) // 2
        if fits(base + text[:mid]):
            low = mid
        else:
            high = mid - 1
    if low >= len(text):
        return text
    sentence_ends = [m.end() for m in _SENTENCE_END.finditer(text) if m.end() <= low]
    if sentence_ends and sentence_ends[-1] >= low // 2:      # don't keep a stub if the sentence end is far back
        return text[:sentence_ends[-1]].rstrip()
    if text[low].isspace():                                  # the cut already falls between words
        return text[:low].rstrip()
    cut = text[:low]
    space = max(cut.rfind(" "), cut.rfind("\n"), cut.rfind("\t"))
    if space > 0:
        return cut[:space].rstrip()
    return cut                                               # unbroken text: nothing better to do


def pack_context(chunks: list[RagChunk], fits: Callable[[str], bool],
                 min_chars: int = 128) -> tuple[str, list[RagChunk]]:
    """Build the retrieval system message from whole passages, in rank order.

    Passages are added while ``fits(candidate)`` holds and packing stops at the first that does not.
    Only when not even the top passage fits whole is that one trimmed (at a sentence end if possible);
    the returned chunk then carries the trimmed text, so citations match what the model saw.
    """
    text = _HEADER
    used: list[RagChunk] = []
    if fits(text):
        for number, chunk in enumerate(chunks, 1):
            candidate = text + _prefix(number, chunk) + chunk.text
            if fits(candidate):
                text, used = candidate, used + [chunk]
                continue
            if not used:
                base = text + _prefix(number, chunk)
                trimmed = _trim_to_fit(base, chunk.text, fits) if fits(base) else ""
                if trimmed:
                    text = base + trimmed
                    used = [chunk.model_copy(update={"text": trimmed})]
            break
    if not used or len(text) < min_chars:
        raise ValueError(NO_ROOM)
    return text, used


def injectable_context(query: str, top_k: Optional[int] = None,
                       min_score: Optional[float] = None,
                       document_ids: Optional[list[str]] = None) -> str:
    """Retrieve and format passages with no length budget."""
    chunks = retrieve_chunks(query, top_k, min_score, document_ids)
    return pack_context(chunks, lambda _text: True)[0]
