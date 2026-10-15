from __future__ import annotations

import os

from fastapi import APIRouter, HTTPException, Request
from fastapi.responses import Response
from pydantic import ValidationError
from backend.request_work import request_work, ClientDisconnected

from backend.rag import embeddings
from backend.rag.chunking import chunk_text, detect_kind, extract_text, extract_content
from backend.rag.schemas import (
    RagConfig,
    RagConfigUpdate,
    RagDocument,
    RagIngestRequest,
    RagQueryRequest,
    RagQueryResponse,
)
from backend.rag.store import RagStore, get_rag_store, set_rag_store
from backend.paths import app_home

rag_router = APIRouter(prefix="/rag", tags=["rag"])


def _get_store() -> RagStore:
    store = get_rag_store()
    if store is None:
        raise HTTPException(status_code=503, detail="RAG not initialized")
    return store


@rag_router.get("/status")
def rag_status():
    store = _get_store()
    cfg = store.get_config()
    docs = store.list_documents()
    return {
        "engine_ready": embeddings.is_available(),
        "model": cfg.model,
        "documents": len(docs),
        "chunks": store.count_chunks(),
        "config": cfg.model_dump(),
    }


# ---- config -------------------------------------------------------------


@rag_router.get("/config", response_model=RagConfig)
def get_config():
    return _get_store().get_config()


@rag_router.post("/config", response_model=RagConfig)
def set_config(cfg: RagConfigUpdate):
    store = _get_store()
    try:
        return store.update_config(cfg.model_dump(exclude_none=True))
    except ValidationError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    except ValueError as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from exc


# ---- documents ----------------------------------------------------------


@rag_router.get("/documents", response_model=list[RagDocument])
def list_documents():
    return _get_store().list_documents()


@rag_router.post("/documents", response_model=RagDocument, status_code=201)
async def ingest_document(body: RagIngestRequest, request: Request):
    try:
        return await request_work(request, lambda cancelled: _ingest_document(body, cancelled))
    except ClientDisconnected:
        return Response(status_code=499)


def _ingest_document(body: RagIngestRequest, cancelled=None):
    store = _get_store()
    if not body.path.strip() and not body.content.strip():
        raise HTTPException(status_code=400, detail="Provide either 'path' or 'content'")
    if not embeddings.is_available():
        raise HTTPException(
            status_code=503,
            detail="RAG embedding needs 'sentence-transformers'. Install with "
            "'uv pip install --python .venv/bin/python -e 'backend[ml]''.",
        )
    if body.content.strip():
        if not body.name.strip():
            raise HTTPException(status_code=400, detail="'name' is required when sending raw content")
        kind = detect_kind(body.name, body.kind)
        try:
            text = extract_content(body.content, kind)
        except ValueError as exc:
            raise HTTPException(status_code=400, detail=str(exc)) from exc
        path = ""
        size_kb = len(body.content.encode("utf-8", "replace")) / 1024.0
    else:
        path = os.path.abspath(body.path)
        if not os.path.isfile(path):
            raise HTTPException(status_code=400, detail=f"Document file not found: {body.path}")
        kind = detect_kind(path, body.kind)
        try:
            text = extract_text(path, kind)
        except ValueError as exc:
            raise HTTPException(status_code=400, detail=str(exc)) from exc
        size_kb = os.path.getsize(path) / 1024.0

    cfg = store.get_config()
    chunk_size = body.chunk_size or cfg.chunk_size
    chunk_overlap = (body.chunk_overlap
                     if body.chunk_overlap is not None else cfg.chunk_overlap)
    try:
        chunks = chunk_text(text, chunk_size, chunk_overlap)
    except ValueError as exc:
        raise HTTPException(status_code=422, detail=str(exc)) from exc
    if not chunks:
        raise HTTPException(status_code=400, detail="Document contains no usable text")

    try:
        vecs = embeddings.embed_texts(chunks, cfg.model)
        if cancelled is not None and cancelled.is_set():
            raise RuntimeError("Document ingestion cancelled before registration")
        rec = store.add_document(
            name=body.name.strip() or os.path.splitext(os.path.basename(path))[0],
            kind=kind,
            path=path,
            model=cfg.model,
            chunks=chunks,
            embeddings=vecs,
            size_kb=size_kb,
            chunk_size=chunk_size,
            chunk_overlap=chunk_overlap,
        )
    except ValueError as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from exc
    except Exception as exc:
        raise HTTPException(status_code=503, detail=f"Document embedding failed: {exc}") from exc
    return rec


@rag_router.delete("/documents/{doc_id}", status_code=204)
def delete_document(doc_id: str):
    if not _get_store().delete_document(doc_id):
        raise HTTPException(status_code=404, detail=f"No document '{doc_id}'")


@rag_router.post("/clear", status_code=204)
def clear_documents():
    _get_store().clear()


# ---- retrieval ----------------------------------------------------------


@rag_router.post("/query", response_model=RagQueryResponse)
def query_documents(body: RagQueryRequest):
    store = _get_store()
    query = body.query.strip()
    if not query:
        raise HTTPException(status_code=400, detail="'query' is required")
    if not embeddings.is_available():
        raise HTTPException(status_code=503, detail="Embeddings unavailable")

    cfg = store.get_config()
    top_k = body.top_k or cfg.top_k
    min_score = body.min_score if body.min_score is not None else cfg.min_score
    try:
        query_vec = embeddings.embed_texts([query], cfg.model)[0]
        results = store.search(query_vec, cfg.model, top_k=top_k, min_score=min_score)
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(status_code=409, detail=str(exc)) from exc
    return RagQueryResponse(query=query, results=results)


def _rag_setup() -> None:
    """Create the RAG store under MAGNITE_HOME (called once at startup)."""
    store = RagStore(os.path.join(app_home(), "rag"))
    set_rag_store(store)
