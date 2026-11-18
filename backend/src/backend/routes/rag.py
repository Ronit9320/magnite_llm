from __future__ import annotations

import logging
import os
import time

import numpy as np

from fastapi import APIRouter, HTTPException, Request
from fastapi.responses import Response
from pydantic import ValidationError
from backend.request_work import request_work, ClientDisconnected

from backend.rag import embeddings, keyword
from backend.rag.chunking import chunk_limit, chunk_text, detect_kind, extract_text, extract_content, fit_chunking
from backend.rag.activity import exclusive, get_activity
from backend.rag.batching import IngestCancelled, embed_batches
from backend.rag.memguard import MemoryGuard, MemoryGuardTripped, limits_from_env
from backend.rag.schemas import (
    RagConfig,
    RagConfigUpdate,
    RagDocument,
    RagIngestRequest,
    RagQueryRequest,
    RagQueryResponse,
    RagReindexRequest,
)
from backend.rag.store import RagStore, get_rag_store, set_rag_store
from backend.paths import app_home
from backend.textcheck import MAX_CONTENT_CHARS, ensure_utf8

rag_router = APIRouter(prefix="/rag", tags=["rag"])
logger = logging.getLogger("backend.rag")


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
    embedder = embeddings.status_for(cfg.model, cfg.truncate_dim)
    size, overlap = fit_chunking(cfg.chunk_size, cfg.chunk_overlap, embedder["max_seq_tokens"])
    return {
        "engine_ready": embeddings.is_available(),
        "model": cfg.model,
        "documents": len(docs),
        "chunks": store.count_chunks(),
        "config": cfg.model_dump(),
        "embedder": embedder,
        "chunking": {"chunk_size": size, "chunk_overlap": overlap,
                     "clamped": (size, overlap) != (cfg.chunk_size, cfg.chunk_overlap),
                     "max_chunk_size": chunk_limit(embedder["max_seq_tokens"])},
        "search": {"mode": "hybrid" if cfg.search_mode == "hybrid" and store.keyword_available else "vector",
                   "requested_mode": cfg.search_mode,
                   "keyword_available": store.keyword_available,
                   "rrf_k": keyword.RRF_K},
    }


@rag_router.get("/activity")
def rag_activity():
    """The running ingest or reindex (phase and progress), for polling clients."""
    return get_activity().snapshot()


@rag_router.get("/embedders")
def list_embedders():
    """Supported embedding models with their limits, prompts and approximate memory use."""
    return embeddings.list_presets()


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


MAX_REFITS = 3


class _SpeedMeter:
    """Times the embedding calls of one ingest/reindex so status can report real chunks/s.

    A call that starts before this model is loaded is not counted (it includes the model load, and
    maybe a download). Nothing is recorded unless the whole run succeeds (``commit``)."""

    def __init__(self, model: str):
        self.model_id = embeddings.spec_for(model).model_id
        self.chunks, self.seconds = 0, 0.0

    def _loaded(self) -> bool:
        info = embeddings.loaded_info()
        return bool(info) and info.get("model") == self.model_id

    def wrap(self, embed_fn):
        def timed(texts: list):
            counted = self._loaded()
            start = time.perf_counter()
            vectors = embed_fn(texts)
            if counted:
                self.chunks += len(texts)
                self.seconds += time.perf_counter() - start
            return vectors
        return timed

    def commit(self) -> None:
        info = embeddings.loaded_info() or {}
        embeddings.record_speed(self.model_id, self.chunks, self.seconds, info.get("device"), info.get("dtype"))


class _NeedsSmallerChunks(Exception):
    """The model's own tokenizer shows these chunks are longer than its limit."""

    def __init__(self, chunk_size: int, chunk_overlap: int):
        self.chunk_size, self.chunk_overlap = chunk_size, chunk_overlap


def _embed_all(chunks: list, cfg, refit_limit, chunk_size: int, chunk_overlap: int, cancelled, progress,
               meter: "_SpeedMeter | None" = None):
    """Embed every chunk in small batches into one preallocated float32 matrix (N x dim): memory is
    that matrix plus one batch, whatever the document size. Nothing is stored until all succeed.

    When ``refit_limit`` is set, the first batch (which loads the model) is followed by a check of real
    token counts; chunks over the limit raise ``_NeedsSmallerChunks`` with a size that will fit."""
    total, batch = len(chunks), _batch_size()
    progress.set_phase("embedding", total)
    matrix = {"vectors": None, "checked": refit_limit is None}

    def write(offset: int, vecs: np.ndarray) -> None:
        if matrix["vectors"] is None:
            matrix["vectors"] = np.empty((total, vecs.shape[1]), dtype=np.float32)
        matrix["vectors"][offset:offset + vecs.shape[0]] = vecs

    def embed(texts: list) -> np.ndarray:
        vecs = embeddings.embed_documents(texts, cfg.model, cfg.truncate_dim)
        if not matrix["checked"]:
            matrix["checked"] = True
            counts = embeddings.count_tokens(chunks, cfg.model)
            if counts and max(counts) > refit_limit:
                worst = max(n / max(1, len(c.split())) for n, c in zip(counts, chunks))
                size = max(16, min(chunk_size - 1, int((refit_limit - 2) / worst * 0.95)))
                raise _NeedsSmallerChunks(size, min(chunk_overlap, size // 4))
        return vecs

    embed_batches((chunks[i:i + batch] for i in range(0, total, batch)), meter.wrap(embed) if meter else embed,
                  write, cancelled, progress.advance, MemoryGuard(limits_from_env()))
    return matrix["vectors"]


def _batch_size() -> int:
    try:
        return max(1, int(os.environ.get("MAGNITE_RAG_BATCH", "16")))
    except ValueError:
        return 16


def _ingest_document(body: RagIngestRequest, cancelled=None):
    store = _get_store()
    if store.reindexing:
        raise HTTPException(status_code=409, detail="Reindex in progress")
    if len(body.content) > MAX_CONTENT_CHARS:
        raise HTTPException(status_code=413, detail=f"Content too long (limit {MAX_CONTENT_CHARS:,} characters)")
    if not body.path.strip() and not body.content.strip():
        raise HTTPException(status_code=400, detail="Provide either 'path' or 'content'")
    if not embeddings.is_available():
        raise HTTPException(
            status_code=503,
            detail="RAG embedding needs 'sentence-transformers'. Install with "
            "'uv pip install --python .venv/bin/python -e 'backend[ml]''.",
        )
    label = body.name.strip() or os.path.basename(body.path.strip()) or "document"
    try:
        # One heavy operation at a time: extraction, chunking and embedding all hold large buffers.
        with exclusive("ingest", label, cancelled) as progress:
            return _ingest_locked(store, body, cancelled, progress)
    except IngestCancelled as exc:
        raise HTTPException(status_code=499, detail=f"Document ingestion cancelled: {exc}") from exc


def _ingest_locked(store, body: RagIngestRequest, cancelled, progress):
    progress.set_phase("extracting")
    if body.content.strip():
        if not body.name.strip():
            raise HTTPException(status_code=400, detail="'name' is required when sending raw content")
        kind = detect_kind(body.name, body.kind)
        try:
            ensure_utf8(body.content, "Document")
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

    try:
        ensure_utf8(text, "Document")   # extraction (e.g. a PDF) can produce one too
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    cfg = store.get_config()
    token_limit = embeddings.status_for(cfg.model, cfg.truncate_dim)["max_seq_tokens"]
    requested = (body.chunk_size or cfg.chunk_size,
                 body.chunk_overlap if body.chunk_overlap is not None else cfg.chunk_overlap)
    chunk_size, chunk_overlap = fit_chunking(requested[0], requested[1], token_limit)
    if (chunk_size, chunk_overlap) != requested:
        logger.info("Chunk size %s words clamped to %s to fit the %s-token limit of %s",
                    requested[0], chunk_size, token_limit, cfg.model)

    meter = _SpeedMeter(cfg.model)
    try:
        for attempt in range(MAX_REFITS + 1):
            progress.set_phase("chunking")
            try:
                chunks = chunk_text(text, chunk_size, chunk_overlap)
            except ValueError as exc:
                raise HTTPException(status_code=422, detail=str(exc)) from exc
            if not chunks:
                raise HTTPException(status_code=400, detail="Document contains no usable text")
            try:
                vectors = _embed_all(chunks, cfg, token_limit if attempt < MAX_REFITS else None,
                                     chunk_size, chunk_overlap, cancelled, progress, meter)
                break
            except _NeedsSmallerChunks as refit:
                logger.info("Chunks exceed the %s-token limit; re-chunking at %d words (was %d)",
                            token_limit, refit.chunk_size, chunk_size)
                chunk_size, chunk_overlap = refit.chunk_size, refit.chunk_overlap
        del text
        counts = embeddings.count_tokens(chunks, cfg.model)
        if counts and token_limit:
            over = sum(1 for n in counts if n > token_limit)
            if over:
                logger.warning("%d of %d chunks exceed the embedding model's token limit (%d); their tails "
                               "were not embedded. Lower the chunk size.", over, len(counts), token_limit)
        if cancelled is not None and cancelled.is_set():
            raise IngestCancelled("before registration")
        progress.set_phase("saving")
        rec = store.add_document(
            name=body.name.strip() or os.path.splitext(os.path.basename(path))[0],
            kind=kind,
            path=path,
            model=cfg.model,
            chunks=chunks,
            embeddings=vectors,
            size_kb=size_kb,
            chunk_size=chunk_size,
            chunk_overlap=chunk_overlap,
        )
        meter.commit()
    except (IngestCancelled, HTTPException):
        raise
    except ValueError as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from exc
    except MemoryGuardTripped as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc
    except Exception as exc:
        raise HTTPException(status_code=503, detail=f"Document embedding failed: {exc}") from exc
    return rec


@rag_router.delete("/documents/{doc_id}", status_code=204)
def delete_document(doc_id: str):
    try:
        deleted = _get_store().delete_document(doc_id)
    except ValueError as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from exc
    if not deleted:
        raise HTTPException(status_code=404, detail=f"No document '{doc_id}'")


@rag_router.post("/clear", status_code=204)
def clear_documents():
    try:
        _get_store().clear()
    except ValueError as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from exc


# ---- reindex ------------------------------------------------------------


@rag_router.post("/reindex", response_model=RagConfig)
async def reindex(body: RagReindexRequest, request: Request):
    """Re-embed every stored chunk with another model (or size) without the original files."""
    try:
        return await request_work(request, lambda cancelled: _reindex(body, cancelled))
    except ClientDisconnected:
        return Response(status_code=499)


def _reindex(body: RagReindexRequest, cancelled=None):
    store = _get_store()
    model = body.model.strip()
    if not model:
        raise HTTPException(status_code=400, detail="'model' is required")
    spec = embeddings.spec_for(model)
    if body.truncate_dim and embeddings.spec_is_known(spec) and not spec.supports_truncation:
        raise HTTPException(status_code=400, detail=f"{model} does not support embedding truncation; use truncate_dim 0")
    if not embeddings.is_available():
        raise HTTPException(status_code=503, detail="Embeddings unavailable; install the ML extras")
    try:
        meter = _SpeedMeter(model)
        with exclusive("reindex", model, cancelled) as progress:
            progress.set_phase("embedding", store.count_chunks())
            result = store.reindex(
                model, body.truncate_dim,
                meter.wrap(lambda texts: embeddings.embed_documents(texts, model, body.truncate_dim)),
                cancelled=cancelled, progress=progress.advance, guard=MemoryGuard(limits_from_env()))
        meter.commit()
        return result
    except IngestCancelled as exc:
        raise HTTPException(status_code=499, detail=str(exc)) from exc
    except MemoryGuardTripped as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc
    except ValueError as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from exc
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(status_code=503, detail=f"Reindex failed: {exc}") from exc


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
        query_vec = embeddings.embed_query(query, cfg.model, cfg.truncate_dim)
        results = store.search(query_vec, cfg.model, top_k=top_k, min_score=min_score,
                               document_ids=body.document_ids, query_text=query,
                               mode=body.search_mode or cfg.search_mode)
    except MemoryGuardTripped as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(status_code=409, detail=str(exc)) from exc
    return RagQueryResponse(query=query, results=results)


def _rag_setup() -> None:
    """Create the RAG store under MAGNITE_HOME (called once at startup)."""
    store = RagStore(os.path.join(app_home(), "rag"))
    set_rag_store(store)
