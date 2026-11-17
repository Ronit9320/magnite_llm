from __future__ import annotations

import os
import sqlite3

from backend.database import connection, open_or_quarantine
import threading
import time
import uuid
from typing import Optional

import numpy as np

from backend.rag.schemas import RagChunk, RagConfig, RagDocument

_DOC_SCHEMA = """
CREATE TABLE IF NOT EXISTS rag_documents (
    id         TEXT PRIMARY KEY,
    name       TEXT NOT NULL,
    kind       TEXT NOT NULL,
    path       TEXT NOT NULL DEFAULT '',
    model      TEXT NOT NULL DEFAULT '',
    chunks     INTEGER NOT NULL DEFAULT 0,
    tokens     INTEGER NOT NULL DEFAULT 0,
    size_kb    REAL NOT NULL DEFAULT 0,
    created_at TEXT NOT NULL DEFAULT ''
);
"""

_CHUNK_SCHEMA = """
CREATE TABLE IF NOT EXISTS rag_chunks (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    doc_id      TEXT NOT NULL,
    chunk_index INTEGER NOT NULL,
    text        TEXT NOT NULL,
    tokens      INTEGER NOT NULL DEFAULT 0,
    embedding   BLOB NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_rag_chunks_doc ON rag_chunks(doc_id);
"""

_CONFIG_SCHEMA = """
CREATE TABLE IF NOT EXISTS rag_config (
    key   TEXT PRIMARY KEY,
    value TEXT NOT NULL
);
"""


def _now() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


_SCAN_BATCH = 10_000        # rows per read when the index is over budget
_SCORE_BLOCK = 8192         # rows scored at a time (converted to float32 per block)
_LOAD_BATCH = 5_000         # rows per read while building the index
_HYDRATE_BATCH = 500        # ids per SELECT ... IN (...)
_INT8_SCALE = 127.0

_DTYPES = {"float16": np.float16, "float32": np.float32, "int8": np.int8}


def _index_dtype() -> np.dtype:
    name = os.environ.get("MAGNITE_RAG_INDEX_DTYPE", "float16").strip().lower()
    return np.dtype(_DTYPES.get(name, np.float16))


def _index_budget_bytes() -> int:
    try:
        mb = float(os.environ.get("MAGNITE_RAG_INDEX_MB", "512"))
    except ValueError:
        mb = 512.0
    return int(max(mb, 0) * 1024 * 1024)


def _candidate_count(top_k: int) -> int:
    """Over-select by approximate score, then rescore exactly from the float32 vectors on disk."""
    return max(2 * top_k, top_k + 16)


def _top_n(scores: np.ndarray, n: int) -> np.ndarray:
    """Positions of the n highest finite scores (unordered)."""
    n = min(n, scores.shape[0])
    if n <= 0:
        return np.empty(0, dtype=np.int64)
    idx = np.argpartition(-scores, n - 1)[:n] if n < scores.shape[0] else np.arange(scores.shape[0])
    return idx[np.isfinite(scores[idx])]


class _SearchIndex:
    """Compact in-memory copy of one model's vectors. Holds no chunk text."""

    def __init__(self, chunk_ids: np.ndarray, doc_codes: np.ndarray, doc_table: list,
                 matrix: np.ndarray):
        self.chunk_ids = chunk_ids          # int64, row -> rag_chunks.id
        self.doc_codes = doc_codes          # int32, row -> index into doc_table
        self.doc_table = doc_table          # list[str] of document ids
        self.matrix = matrix                # (N, D) float16 | float32 | int8

    @property
    def size(self) -> int:
        return int(self.matrix.shape[0])

    @property
    def dim(self) -> int:
        return int(self.matrix.shape[1]) if self.matrix.ndim == 2 else 0

    def doc_mask(self, document_ids) -> np.ndarray:
        wanted = set(document_ids)
        codes = [i for i, d in enumerate(self.doc_table) if d in wanted]
        return np.isin(self.doc_codes, codes)

    def top_candidates(self, query: np.ndarray, mask: Optional[np.ndarray], n: int) -> np.ndarray:
        """Row indices of the n best rows by approximate score (matrix blocks scored in float32)."""
        best_scores, best_rows = [], []
        quantized = self.matrix.dtype == np.int8
        for start in range(0, self.size, _SCORE_BLOCK):
            block = self.matrix[start:start + _SCORE_BLOCK].astype(np.float32, copy=False)
            scores = block @ query
            if quantized:
                scores = scores / _INT8_SCALE
            if mask is not None:
                scores = np.where(mask[start:start + _SCORE_BLOCK], scores, -np.inf)
            keep = _top_n(scores, n)
            best_scores.append(scores[keep])
            best_rows.append(keep + start)
        if not best_rows:
            return np.empty(0, dtype=np.int64)
        scores = np.concatenate(best_scores)
        rows = np.concatenate(best_rows)
        keep = _top_n(scores, n)
        return rows[keep]


class RagStore:
    """SQLite-backed document registry + exact vector search.

    Embeddings are stored per chunk as float32 blobs (the source of truth). Search keeps a derived,
    compact in-memory index (float16 by default, capped by MAGNITE_RAG_INDEX_MB), picks candidates
    by approximate score, then rescores them exactly from the float32 vectors and loads text only
    for the hits. Every write drops the index; it is rebuilt lazily, so nothing needs syncing.
    Over the memory cap, search streams the vectors from SQLite in batches instead. Thread-safe.
    """

    def __init__(self, data_dir: str):
        self._data_dir = data_dir
        os.makedirs(data_dir, exist_ok=True)
        self._db_path = os.path.join(data_dir, "rag.db")
        self._lock = threading.RLock()
        self._indexes: dict = {}          # model -> _SearchIndex (derived; rebuilt lazily)
        self._init_db()

    def _init_db(self) -> None:
        def init(conn):
            conn.execute(_DOC_SCHEMA)
            conn.executescript(_CHUNK_SCHEMA)
            conn.execute(_CONFIG_SCHEMA)

        with self._lock:
            open_or_quarantine(self._db_path, init, hint="Knowledge documents must be added again.")

    def _conn(self):
        return connection(self._db_path)

    # ---- config ---------------------------------------------------------

    def get_config(self) -> RagConfig:
        cfg = RagConfig()
        with self._lock, self._conn() as conn:
            rows = conn.execute("SELECT key, value FROM rag_config").fetchall()
            existing = conn.execute("SELECT model FROM rag_documents WHERE model != '' LIMIT 1").fetchone()
        if not any(row["key"] == "model" for row in rows):
            # Nothing saved yet. An existing library keeps the model it was built with, so changing the
            # recommended default never hides it; a fresh store gets the recommendation for this machine.
            if existing is not None:
                cfg.model = existing["model"]
            else:
                from backend.rag import embeddings
                cfg.model = embeddings.recommended_model()
        for row in rows:
            key, value = row["key"], row["value"]
            if not hasattr(cfg, key):
                continue
            current = getattr(cfg, key)
            try:
                setattr(cfg, key, type(current)(value))
            except (TypeError, ValueError):
                pass
        return cfg

    def set_config(self, cfg: RagConfig) -> RagConfig:
        with self._lock, self._conn() as conn:
            if conn.execute("SELECT 1 FROM rag_documents WHERE model != ? LIMIT 1", (cfg.model,)).fetchone():
                raise ValueError("Clear and re-ingest documents before changing embedding models")
            if (cfg.truncate_dim != self.get_config().truncate_dim
                    and conn.execute("SELECT 1 FROM rag_documents LIMIT 1").fetchone()):
                raise ValueError("Clear and re-ingest documents before changing the embedding size")
            for key, value in cfg.model_dump().items():
                conn.execute(
                    "INSERT INTO rag_config (key, value) VALUES (?, ?) "
                    "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                    (key, str(value)),
                )
            conn.commit()
        return self.get_config()

    def update_config(self, changes: dict) -> RagConfig:
        with self._lock:
            cfg = RagConfig.model_validate({**self.get_config().model_dump(), **changes})
            return self.set_config(cfg)

    # ---- documents ------------------------------------------------------

    def add_document(self, name: str, kind: str, path: str, model: str,
                     chunks: list[str], embeddings: np.ndarray, size_kb: float,
                     chunk_size: int, chunk_overlap: int) -> RagDocument:
        """Store a document and its embedded chunks (all-or-nothing)."""
        if not chunks or embeddings.shape[0] != len(chunks):
            raise ValueError("Embedding count must match chunk count")
        if embeddings.shape[0] == 0:
            raise ValueError("Document produced no chunks")

        doc_id = uuid.uuid4().hex[:12]
        tokens = sum(len(c.split()) for c in chunks)

        with self._lock:
            if model != self.get_config().model:
                raise ValueError("Embedding model changed during ingestion; retry with the current configuration")
            dim = embeddings.shape[1]
            existing_dim = self._embedding_dim()
            if existing_dim is not None and existing_dim != dim:
                raise ValueError(
                    f"Embedding dimension {dim} does not match existing index "
                    f"dimension {existing_dim}. Docs were embedded with a "
                    "different model."
                )
            with self._conn() as conn:
                conn.execute(
                    """
                    INSERT INTO rag_documents (id, name, kind, path, model, chunks,
                                               tokens, size_kb, created_at)
                    VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)
                    """,
                    (doc_id, name, kind, path, model, len(chunks), tokens,
                     round(size_kb, 1), _now()),
                )
                conn.executemany(
                    """
                    INSERT INTO rag_chunks (doc_id, chunk_index, text, tokens, embedding)
                    VALUES (?, ?, ?, ?, ?)
                    """,
                    [
                        (doc_id, i, chunks[i], len(chunks[i].split()),
                         embeddings[i].astype(np.float32).tobytes())
                        for i in range(len(chunks))
                    ],
                )
                conn.commit()
            self._invalidate()

        return RagDocument(
            id=doc_id, name=name, kind=kind, path=path, chunks=len(chunks),
            tokens=tokens, size_kb=round(size_kb, 1), created_at=_now(),
        )

    def _embedding_dim(self) -> Optional[int]:
        with self._conn() as conn:
            row = conn.execute("SELECT embedding FROM rag_chunks LIMIT 1").fetchone()
        if row is None:
            return None
        return len(np.frombuffer(row["embedding"], dtype=np.float32))

    def list_documents(self) -> list[RagDocument]:
        with self._lock, self._conn() as conn:
            rows = conn.execute(
                "SELECT * FROM rag_documents ORDER BY created_at DESC"
            ).fetchall()
        return [self._from_row(r) for r in rows]

    def get_document(self, doc_id: str) -> Optional[RagDocument]:
        with self._lock, self._conn() as conn:
            row = conn.execute(
                "SELECT * FROM rag_documents WHERE id = ?", (doc_id,)
            ).fetchone()
        return self._from_row(row) if row else None

    def delete_document(self, doc_id: str) -> bool:
        with self._lock, self._conn() as conn:
            cur = conn.execute("DELETE FROM rag_documents WHERE id = ?", (doc_id,))
            conn.execute("DELETE FROM rag_chunks WHERE doc_id = ?", (doc_id,))
            conn.commit()
            self._invalidate()
            return cur.rowcount > 0

    def clear(self) -> None:
        with self._lock, self._conn() as conn:
            conn.execute("DELETE FROM rag_chunks")
            conn.execute("DELETE FROM rag_documents")
            conn.commit()
            self._invalidate()

    def count_chunks(self) -> int:
        with self._lock, self._conn() as conn:
            row = conn.execute("SELECT COUNT(*) AS n FROM rag_chunks").fetchone()
        return int(row["n"])

    # ---- retrieval ------------------------------------------------------

    def _invalidate(self) -> None:
        with self._lock:
            self._indexes.clear()

    def _index_for(self, model: str) -> Optional[_SearchIndex]:
        """The cached index for a model, or None when it would exceed the memory budget."""
        with self._lock:
            cached = self._indexes.get(model)
            if cached is not None:
                return cached
            index = self._build_index(model)
            if index is not None:
                self._indexes[model] = index
            return index

    def _build_index(self, model: str) -> Optional[_SearchIndex]:
        dtype = _index_dtype()
        with self._lock, self._conn() as conn:
            n = conn.execute(
                "SELECT COUNT(*) AS n FROM rag_chunks c JOIN rag_documents d ON d.id = c.doc_id "
                "WHERE d.model = ?", (model,)).fetchone()["n"]
            if n == 0:
                return _SearchIndex(np.empty(0, np.int64), np.empty(0, np.int32), [],
                                    np.empty((0, 0), dtype))
            first = conn.execute(
                "SELECT length(c.embedding) AS b FROM rag_chunks c JOIN rag_documents d "
                "ON d.id = c.doc_id WHERE d.model = ? LIMIT 1", (model,)).fetchone()
            dim = int(first["b"]) // 4
            if n * dim * dtype.itemsize > _index_budget_bytes():
                return None
            chunk_ids = np.empty(n, np.int64)
            doc_codes = np.empty(n, np.int32)
            matrix = np.empty((n, dim), dtype)
            doc_table: list = []
            codes: dict = {}
            cursor = conn.execute(
                "SELECT c.id, c.doc_id, c.embedding FROM rag_chunks c JOIN rag_documents d "
                "ON d.id = c.doc_id WHERE d.model = ? ORDER BY c.id", (model,))
            pos = 0
            while True:
                rows = cursor.fetchmany(_LOAD_BATCH)
                if not rows:
                    break
                block = self._stack(rows, dim)
                if dtype == np.int8:
                    block = np.clip(np.rint(block * _INT8_SCALE), -127, 127)
                matrix[pos:pos + len(rows)] = block
                for i, row in enumerate(rows):
                    chunk_ids[pos + i] = row["id"]
                    code = codes.get(row["doc_id"])
                    if code is None:
                        code = codes[row["doc_id"]] = len(doc_table)
                        doc_table.append(row["doc_id"])
                    doc_codes[pos + i] = code
                pos += len(rows)
            return _SearchIndex(chunk_ids[:pos], doc_codes[:pos], doc_table, matrix[:pos])

    @staticmethod
    def _stack(rows, dim: int) -> np.ndarray:
        block = np.empty((len(rows), dim), np.float32)
        for i, row in enumerate(rows):
            vec = np.frombuffer(row["embedding"], dtype=np.float32)
            if vec.shape[0] != dim:
                raise ValueError(
                    f"Stored embedding dimension {vec.shape[0]} does not match index dimension {dim}; "
                    "re-ingest the documents with a single embedding model.")
            block[i] = vec
        return block

    def _scan_candidates(self, model: str, query: np.ndarray, allowed: Optional[set], n: int) -> list:
        """Over-budget path: stream vectors from SQLite in batches, keeping the best n chunk ids."""
        best_scores = np.empty(0, np.float32)
        best_ids = np.empty(0, np.int64)
        with self._conn() as conn:
            cursor = conn.execute(
                "SELECT c.id, c.doc_id, c.embedding FROM rag_chunks c JOIN rag_documents d "
                "ON d.id = c.doc_id WHERE d.model = ? ORDER BY c.id", (model,))
            while True:
                rows = cursor.fetchmany(_SCAN_BATCH)
                if not rows:
                    break
                block = self._stack(rows, query.shape[0])
                scores = block @ query
                if allowed is not None:
                    keep_rows = np.fromiter((r["doc_id"] in allowed for r in rows), bool, len(rows))
                    scores = np.where(keep_rows, scores, -np.inf)
                ids = np.fromiter((r["id"] for r in rows), np.int64, len(rows))
                best_scores = np.concatenate([best_scores, scores])
                best_ids = np.concatenate([best_ids, ids])
                keep = _top_n(best_scores, n)
                best_scores, best_ids = best_scores[keep], best_ids[keep]
        return [int(i) for i in best_ids]

    def _hydrate(self, chunk_ids: list) -> list:
        """Text, names and float32 vectors for just these chunks."""
        rows = []
        with self._conn() as conn:
            for start in range(0, len(chunk_ids), _HYDRATE_BATCH):
                part = chunk_ids[start:start + _HYDRATE_BATCH]
                marks = ",".join("?" * len(part))
                rows += conn.execute(
                    "SELECT c.id, c.doc_id, c.chunk_index, c.text, c.tokens, c.embedding, "
                    f"d.name AS doc_name FROM rag_chunks c JOIN rag_documents d ON d.id = c.doc_id "
                    f"WHERE c.id IN ({marks})", part).fetchall()
        return rows

    def search(self, query_vec: np.ndarray, model: str, top_k: int = 4,
               min_score: float = 0.0, document_ids: Optional[list[str]] = None) -> list[RagChunk]:
        """Exact cosine search over chunks embedded with ``model`` (vectors are L2-normalized)."""
        if document_ids is not None and not document_ids:
            return []
        query = np.asarray(query_vec, dtype=np.float32).reshape(-1)
        wanted = _candidate_count(top_k)
        with self._lock:
            index = self._index_for(model)
            if index is not None:
                if index.size == 0:
                    return []
                if query.shape[0] != index.dim:
                    raise ValueError(
                        f"Query embedding dimension {query.shape[0]} does not match the index "
                        f"dimension {index.dim}; the embedding model changed.")
                mask = None if document_ids is None else index.doc_mask(document_ids)
                rows = index.top_candidates(query, mask, wanted)
                ids = [int(i) for i in index.chunk_ids[rows]]
            else:
                allowed = None if document_ids is None else set(document_ids)
                ids = self._scan_candidates(model, query, allowed, wanted)
            if not ids:
                return []
            hydrated = self._hydrate(ids)
            if not hydrated:
                return []
            scores = np.array([float(np.frombuffer(r["embedding"], dtype=np.float32) @ query)
                               for r in hydrated])
            row_ids = np.array([r["id"] for r in hydrated], dtype=np.int64)
        order = np.lexsort((row_ids, -scores))          # best score first, ties by chunk id
        results: list[RagChunk] = []
        for pos in order:
            score = float(scores[pos])
            if min_score > 0 and score < min_score:
                break
            row = hydrated[pos]
            results.append(RagChunk(
                document_id=row["doc_id"], document_name=row["doc_name"],
                index=int(row["chunk_index"]), text=row["text"], tokens=int(row["tokens"]),
                score=round(score, 4)))
            if len(results) >= top_k:
                break
        return results

    @staticmethod
    def _from_row(row: sqlite3.Row) -> RagDocument:
        return RagDocument(
            id=row["id"],
            name=row["name"],
            kind=row["kind"],
            path=row["path"],
            chunks=row["chunks"],
            tokens=row["tokens"],
            size_kb=row["size_kb"],
            created_at=row["created_at"],
        )


_rag_store: RagStore | None = None


def set_rag_store(store: RagStore) -> None:
    global _rag_store
    _rag_store = store


def get_rag_store() -> Optional[RagStore]:
    return _rag_store
