from __future__ import annotations

import os
import sqlite3
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


class RagStore:
    """SQLite-backed document registry + flat (FAISS-style) vector store.

    Embeddings are stored per chunk as float32 blobs; queries do a full scan
    with a dot product over L2-normalized vectors (equivalently cosine
    similarity), which is what a flat FAISS index does and is plenty for a
    local knowledge base of a few thousand chunks. Thread-safe.
    """

    def __init__(self, data_dir: str):
        self._data_dir = data_dir
        os.makedirs(data_dir, exist_ok=True)
        self._db_path = os.path.join(data_dir, "rag.db")
        self._lock = threading.Lock()
        self._init_db()

    def _init_db(self) -> None:
        with self._lock, sqlite3.connect(self._db_path) as conn:
            conn.execute(_DOC_SCHEMA)
            conn.executescript(_CHUNK_SCHEMA)
            conn.execute(_CONFIG_SCHEMA)
            conn.commit()

    def _conn(self) -> sqlite3.Connection:
        conn = sqlite3.connect(self._db_path)
        conn.row_factory = sqlite3.Row
        return conn

    # ---- config ---------------------------------------------------------

    def get_config(self) -> RagConfig:
        cfg = RagConfig()
        with self._lock, self._conn() as conn:
            rows = conn.execute("SELECT key, value FROM rag_config").fetchall()
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
            for key, value in cfg.model_dump().items():
                conn.execute(
                    "INSERT INTO rag_config (key, value) VALUES (?, ?) "
                    "ON CONFLICT(key) DO UPDATE SET value=excluded.value",
                    (key, str(value)),
                )
            conn.commit()
        return self.get_config()

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

        return RagDocument(
            id=doc_id, name=name, kind=kind, path=path, chunks=len(chunks),
            tokens=tokens, size_kb=round(size_kb, 1), created_at=_now(),
        )

    def _embedding_dim(self) -> Optional[int]:
        row = self._conn().execute("SELECT embedding FROM rag_chunks LIMIT 1").fetchone()
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
            return cur.rowcount > 0

    def clear(self) -> None:
        with self._lock, self._conn() as conn:
            conn.execute("DELETE FROM rag_chunks")
            conn.execute("DELETE FROM rag_documents")
            conn.commit()

    def count_chunks(self) -> int:
        with self._lock, self._conn() as conn:
            row = conn.execute("SELECT COUNT(*) AS n FROM rag_chunks").fetchone()
        return int(row["n"])

    # ---- retrieval ------------------------------------------------------

    def search(self, query_vec: np.ndarray, model: str, top_k: int = 4,
               min_score: float = 0.0) -> list[RagChunk]:
        """Flat cosine search over chunks embedded with ``model``."""
        with self._lock, self._conn() as conn:
            rows = conn.execute(
                """
                SELECT c.id, c.doc_id, c.chunk_index, c.text, c.tokens, c.embedding,
                       d.name AS doc_name
                FROM rag_chunks c
                JOIN rag_documents d ON d.id = c.doc_id
                WHERE d.model = ?
                ORDER BY c.id
                """,
                (model,),
            ).fetchall()
        if not rows:
            return []

        matrix = np.stack(
            [np.frombuffer(r["embedding"], dtype=np.float32) for r in rows],
            axis=0,
        )
        scores = matrix @ query_vec.reshape(-1)
        order = np.argsort(-scores)
        results: list[RagChunk] = []
        for idx in order:
            score = float(scores[idx])
            if min_score > 0 and score < min_score:
                break
            row = rows[idx]
            results.append(
                RagChunk(
                    document_id=row["doc_id"],
                    document_name=row["doc_name"],
                    index=int(row["chunk_index"]),
                    text=row["text"],
                    tokens=int(row["tokens"]),
                    score=round(score, 4),
                )
            )
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