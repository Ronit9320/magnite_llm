from __future__ import annotations

from typing import Optional

from backend.rag.schemas import RagChunk

from backend.rag.embeddings import embed_texts, is_available
from backend.rag.store import get_rag_store


def injectable_context(query: str, top_k: Optional[int] = None,
                       min_score: Optional[float] = None,
                       document_ids: Optional[list[str]] = None,
                       sources: Optional[list[tuple[int, RagChunk]]] = None) -> str:
    """Retrieve passages, reporting failures instead of silently answering without RAG."""
    store = get_rag_store()
    if store is None or not is_available():
        raise RuntimeError("RAG is unavailable. Install the ML extras or disable document retrieval.")
    if store.count_chunks() == 0:
        raise ValueError("No RAG documents are indexed. Add documents or disable document retrieval.")

    cfg = store.get_config()
    top_k = top_k if top_k is not None else cfg.top_k
    min_score = min_score if min_score is not None else cfg.min_score
    query_vec = embed_texts([query], cfg.model)[0]
    chunks = store.search(query_vec, cfg.model, top_k=top_k, min_score=min_score, document_ids=document_ids)
    if not chunks:
        raise ValueError("No matching document passages. Lower the similarity threshold or disable retrieval.")

    context = (
        "The user has enabled retrieval over their local documents. Prefer "
        "these retrieved passages when answering:"
    )
    for i, chunk in enumerate(chunks, 1):
        context += f"\n\n[{i}] (from {chunk.document_name}) "
        if sources is not None:
            sources.append((len(context), chunk))
        context += chunk.text
    return context