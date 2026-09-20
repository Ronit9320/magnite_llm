from __future__ import annotations

from typing import Optional

from backend.rag.embeddings import embed_texts, is_available
from backend.rag.store import RagStore, get_rag_store


def injectable_context(query: str, top_k: Optional[int] = None,
                       min_score: Optional[float] = None) -> Optional[str]:
    """Retrieve passages for ``query`` and format them as a system prompt.

    Returns ``None`` (so chat proceeds without RAG) whenever the store is
    uninitialized, the embedder is missing, no docs have been ingested, or any
    retrieval step fails — RAG must never break plain chat.
    """
    store = get_rag_store()
    if store is None or not is_available():
        return None
    if store.count_chunks() == 0:
        return None

    cfg = store.get_config()
    top_k = top_k if top_k is not None else cfg.top_k
    min_score = min_score if min_score is not None else cfg.min_score
    try:
        query_vec = embed_texts([query], cfg.model)[0]
    except Exception:  # noqa: BLE001
        return None

    try:
        chunks = store.search(query_vec, cfg.model, top_k=top_k, min_score=min_score)
    except Exception:  # noqa: BLE001
        return None
    if not chunks:
        return None

    passages = "\n\n".join(
        f"[{i + 1}] (from {c.document_name}) {c.text}" for i, c in enumerate(chunks)
    )
    return (
        "The user has enabled retrieval over their local documents. Prefer "
        "these retrieved passages when answering:\n\n" + passages
    )