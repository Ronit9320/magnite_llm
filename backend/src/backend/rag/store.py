from __future__ import annotations

import contextlib
import glob
import json
import logging
import math
import os
import sqlite3

from backend.database import connection, open_or_quarantine
import threading
import time
import uuid
from typing import Optional

import numpy as np

from backend.rag import keyword
from backend.rag.batching import IngestCancelled, NON_FINITE, embed_batches
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

_FTS_SCHEMA = (
    f"CREATE VIRTUAL TABLE IF NOT EXISTS {keyword.FTS_TABLE} USING fts5("
    "text, content='rag_chunks', content_rowid='id', tokenize='unicode61 remove_diacritics 2')"
)

_FTS_STATE_SCHEMA = """
CREATE TABLE IF NOT EXISTS rag_fts_state (
    key   TEXT PRIMARY KEY,
    value INTEGER NOT NULL
);
"""

log = logging.getLogger(__name__)


class StoreBusy(RuntimeError):
    """The knowledge database is locked by another program (or connection) for longer than the wait."""

    def __init__(self):
        super().__init__("The Knowledge database is busy: another program is using it. "
                         "Close anything that has it open, then try again.")


def _db_timeout() -> float:
    """Seconds to wait for a lock held by someone else before reporting the database as busy."""
    try:
        return max(0.0, float(os.environ.get("MAGNITE_RAG_DB_TIMEOUT", "5")))
    except ValueError:
        return 5.0


def _now() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


_SCAN_BATCH = 10_000        # rows per read when the index is over budget
_SCORE_BLOCK = 8192         # rows scored at a time (converted to float32 per block)
_LOAD_BATCH = 5_000         # rows per read while building the index
_HYDRATE_BATCH = 500        # ids per SELECT ... IN (...)
_INT8_SCALE = 127.0
MAX_TOP_K = 500             # most passages one search returns, whatever the caller asks
_SAVE_BATCH = 2_000         # chunk rows inserted between cancel checks while saving a document

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
        self._reindexing = False
        self._fts_ok = False              # FTS5 usable (module present, table created, rebuilds working)
        self._fts_synced = False          # the keyword index was verified against rag_chunks since the last write
        self._init_db()
        for stale in glob.glob(os.path.join(data_dir, "reindex-*.tmp")):     # left by a crashed reindex
            try:
                os.remove(stale)
            except OSError:
                pass

    def _init_db(self) -> None:
        def init(conn):
            conn.execute(_DOC_SCHEMA)
            conn.executescript(_CHUNK_SCHEMA)
            conn.execute(_CONFIG_SCHEMA)
            if keyword.fts5_available(conn):
                try:
                    conn.execute(_FTS_SCHEMA)
                    conn.execute(_FTS_STATE_SCHEMA)
                    self._fts_ok = True
                except sqlite3.Error as exc:
                    log.warning("Keyword index unavailable, using vector search only: %s", exc)

        with self._lock:
            open_or_quarantine(self._db_path, init, hint="Knowledge documents must be added again.")

    @contextlib.contextmanager
    def _conn(self):
        try:
            with connection(self._db_path, timeout=_db_timeout()) as conn:
                yield conn
        except sqlite3.OperationalError as exc:
            if "locked" in str(exc).lower() or "busy" in str(exc).lower():
                raise StoreBusy() from exc
            raise

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
        if cfg.search_mode not in ("hybrid", "vector"):       # a value this build does not know
            cfg.search_mode = "hybrid"
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
                     chunk_size: int, chunk_overlap: int, cancelled=None) -> RagDocument:
        """Store a document and its embedded chunks (all-or-nothing).

        ``cancelled`` (a threading.Event) is checked between row batches and right before the commit;
        if it is set the whole transaction is rolled back and ``IngestCancelled`` is raised.
        """
        def check_cancelled() -> None:
            if cancelled is not None and cancelled.is_set():
                raise IngestCancelled("Cancelled while saving the document")

        if not chunks or embeddings.shape[0] != len(chunks):
            raise ValueError("Embedding count must match chunk count")
        if embeddings.shape[0] == 0:
            raise ValueError("Document produced no chunks")
        if not np.isfinite(embeddings).all():
            raise ValueError(NON_FINITE)

        doc_id = uuid.uuid4().hex[:12]
        tokens = sum(len(c.split()) for c in chunks)

        with self._lock:
            self._refuse_during_reindex()
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
            fts = self._ensure_keyword_index()
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
                for start in range(0, len(chunks), _SAVE_BATCH):
                    check_cancelled()
                    conn.executemany(
                        """
                        INSERT INTO rag_chunks (doc_id, chunk_index, text, tokens, embedding)
                        VALUES (?, ?, ?, ?, ?)
                        """,
                        [
                            (doc_id, i, chunks[i], len(chunks[i].split()),
                             embeddings[i].astype(np.float32).tobytes())
                            for i in range(start, min(start + _SAVE_BATCH, len(chunks)))
                        ],
                    )
                check_cancelled()
                if fts:
                    self._fts_apply(conn, (
                        (f"INSERT INTO {keyword.FTS_TABLE} (rowid, text) SELECT id, text FROM rag_chunks "
                         "WHERE doc_id = ?", (doc_id,)),))
                check_cancelled()
                chunk_ids = [r["id"] for r in conn.execute(
                    "SELECT id FROM rag_chunks WHERE doc_id = ? ORDER BY chunk_index", (doc_id,))] \
                    if model in self._indexes else []
                conn.commit()
            self._extend_index(model, doc_id, embeddings, chunk_ids)

        return RagDocument(
            id=doc_id, name=name, kind=kind, path=path, chunks=len(chunks),
            tokens=tokens, size_kb=round(size_kb, 1), created_at=_now(),
        )

    def find_duplicate(self, chunks: list[str]) -> Optional[RagDocument]:
        """The stored document made of exactly these chunks, if there is one. Used to refuse adding the
        same document twice. A cheap query (count, first and last chunk, total length) finds candidates;
        only those are then compared chunk by chunk."""
        if not chunks:
            return None
        with self._lock, self._conn() as conn:
            candidates = conn.execute(
                "SELECT d.* FROM rag_documents d "
                "JOIN rag_chunks a ON a.doc_id = d.id AND a.chunk_index = 0 "
                "JOIN rag_chunks z ON z.doc_id = d.id AND z.chunk_index = ? "
                "WHERE d.chunks = ? AND a.text = ? AND z.text = ? "
                "AND (SELECT SUM(length(text)) FROM rag_chunks WHERE doc_id = d.id) = ? LIMIT 20",
                (len(chunks) - 1, len(chunks), chunks[0], chunks[-1], sum(len(c) for c in chunks))).fetchall()
            for row in candidates:
                stored = [r["text"] for r in conn.execute(
                    "SELECT text FROM rag_chunks WHERE doc_id = ? ORDER BY chunk_index", (row["id"],))]
                if stored == chunks:
                    return self._from_row(row)
        return None

    @staticmethod
    def _modal_dim(conn, model: Optional[str] = None) -> Optional[int]:
        """The embedding size most stored vectors have (float32 each), so a damaged row can never decide it."""
        sql = "SELECT length(c.embedding) AS b, COUNT(*) AS n FROM rag_chunks c "
        params: tuple = ()
        if model is not None:
            sql += "JOIN rag_documents d ON d.id = c.doc_id WHERE d.model = ? AND length(c.embedding) % 4 = 0 "
            params = (model,)
        else:
            sql += "WHERE length(c.embedding) % 4 = 0 "
        row = conn.execute(sql + "GROUP BY b ORDER BY n DESC, b LIMIT 1", params).fetchone()
        return int(row["b"]) // 4 if row and row["b"] else None

    def _embedding_dim(self) -> Optional[int]:
        with self._conn() as conn:
            return self._modal_dim(conn)

    @staticmethod
    def _readable(rows, dim: int) -> list:
        """The rows whose stored vector has the right size. A damaged row is skipped (and logged), so it
        cannot take every search and every ingest down with it."""
        good = [row for row in rows if len(row["embedding"]) == dim * 4]
        if len(good) != len(rows):
            log.warning("Skipping %d chunk(s) whose stored embedding is damaged (wrong size)", len(rows) - len(good))
        return good

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
        with self._lock:
            self._refuse_during_reindex()
            fts = self._ensure_keyword_index()
            with self._conn() as conn:
                if fts:
                    fts = self._fts_apply(conn, (
                        (f"INSERT INTO {keyword.FTS_TABLE} ({keyword.FTS_TABLE}, rowid, text) "
                         "SELECT 'delete', id, text FROM rag_chunks WHERE doc_id = ?", (doc_id,)),))
                cur = conn.execute("DELETE FROM rag_documents WHERE id = ?", (doc_id,))
                conn.execute("DELETE FROM rag_chunks WHERE doc_id = ?", (doc_id,))
                if fts:                           # a failed FTS step must not be hidden by a fresh state row
                    self._fts_apply(conn, ())
                conn.commit()
                self._shrink_index(doc_id)
                return cur.rowcount > 0

    def clear(self) -> None:
        with self._lock:
            self._refuse_during_reindex()
            fts = self._ensure_keyword_index()
            with self._conn() as conn:
                if fts:
                    fts = self._fts_apply(conn, (
                        (f"INSERT INTO {keyword.FTS_TABLE} ({keyword.FTS_TABLE}) VALUES ('delete-all')", ()),))
                conn.execute("DELETE FROM rag_chunks")
                conn.execute("DELETE FROM rag_documents")
                if fts:
                    self._fts_apply(conn, ())
                conn.commit()
                self._invalidate()

    def count_chunks(self) -> int:
        with self._lock, self._conn() as conn:
            row = conn.execute("SELECT COUNT(*) AS n FROM rag_chunks").fetchone()
        return int(row["n"])

    # ---- retrieval ------------------------------------------------------

    # ---- reindex --------------------------------------------------------

    @property
    def reindexing(self) -> bool:
        return self._reindexing

    def _refuse_during_reindex(self) -> None:
        if self._reindexing:
            raise ValueError("Reindex in progress")

    def _chunk_page(self, after_id: int, limit: int) -> list:
        with self._conn() as conn:
            rows = conn.execute("SELECT id, text FROM rag_chunks WHERE id > ? ORDER BY id LIMIT ?",
                                (after_id, limit)).fetchall()
        return [(r["id"], r["text"]) for r in rows]

    def reindex(self, model: str, truncate_dim: int, embed_fn, cancelled=None, progress=None,
                batch: int = 64, guard=None) -> RagConfig:
        """Re-embed every stored chunk with ``model`` and switch the library to it, all or nothing.

        New vectors are staged in a temporary on-disk array (bounded memory), then written in one
        transaction together with every document's model and the config. Until that commit, search
        keeps serving the old vectors; any failure or cancellation leaves the library untouched.
        Adding, deleting and clearing are refused while this runs.
        """
        with self._lock:
            self._refuse_during_reindex()
            cfg = self.get_config()
            with self._conn() as conn:
                total = conn.execute("SELECT COUNT(*) AS n FROM rag_chunks").fetchone()["n"]
                other = conn.execute("SELECT 1 FROM rag_documents WHERE model != ? LIMIT 1", (model,)).fetchone()
            if total == 0:
                return self.update_config({"model": model, "truncate_dim": truncate_dim})
            if model == cfg.model and truncate_dim == cfg.truncate_dim and other is None:
                return cfg
            self._reindexing = True
        path = os.path.join(self._data_dir, f"reindex-{uuid.uuid4().hex[:8]}.tmp")
        staged = {"array": None}
        ids: list = []

        def batches():
            last = 0
            while True:
                page = self._chunk_page(last, batch)
                if not page:
                    return
                last = page[-1][0]
                ids.extend(chunk_id for chunk_id, _ in page)
                yield [text for _, text in page]

        def write(offset: int, vectors: np.ndarray) -> None:
            if staged["array"] is None:
                staged["array"] = np.lib.format.open_memmap(path, mode="w+", dtype=np.float32,
                                                            shape=(total, vectors.shape[1]))
            elif vectors.shape[1] != staged["array"].shape[1]:
                raise ValueError("Embedding dimension changed between batches")
            staged["array"][offset:offset + vectors.shape[0]] = vectors

        try:
            embed_batches(batches(), embed_fn, write, cancelled, progress, guard)
            array = staged["array"]
            if array is None or len(ids) != total:
                raise ValueError("The library changed during reindex; nothing was applied")
            array.flush()
            self._apply_reindex(array, ids, model, truncate_dim)
            return self.get_config()
        finally:
            staged["array"] = None
            try:
                os.remove(path)
            except OSError:
                pass
            with self._lock:
                self._reindexing = False

    def relabel_model(self, model: str) -> RagConfig:
        """Switch the library to another spelling of the model it already uses (e.g. the short and the full
        Hugging Face id): the vectors are the same, so only the stored names change, in one transaction."""
        with self._lock:
            self._refuse_during_reindex()
            with self._conn() as conn:
                conn.execute("UPDATE rag_documents SET model = ?", (model,))
                conn.execute("INSERT INTO rag_config (key, value) VALUES ('model', ?) "
                             "ON CONFLICT(key) DO UPDATE SET value=excluded.value", (model,))
            self._invalidate()
        return self.get_config()

    def _apply_reindex(self, array: np.ndarray, ids: list, model: str, truncate_dim: int) -> None:
        with self._lock:
            with self._conn() as conn:               # commits on success, rolls back on any error
                for start in range(0, len(ids), 1000):
                    stop = min(start + 1000, len(ids))
                    conn.executemany("UPDATE rag_chunks SET embedding = ? WHERE id = ?",
                                     [(array[i].tobytes(), ids[i]) for i in range(start, stop)])
                conn.execute("UPDATE rag_documents SET model = ?", (model,))
                for key, value in (("model", model), ("truncate_dim", str(truncate_dim))):
                    conn.execute("INSERT INTO rag_config (key, value) VALUES (?, ?) "
                                 "ON CONFLICT(key) DO UPDATE SET value=excluded.value", (key, value))
            self._invalidate()

    def _invalidate(self) -> None:
        with self._lock:
            self._indexes.clear()
            self._fts_synced = False        # re-verified (cheaply) before the next keyword search or write

    def _extend_index(self, model: str, doc_id: str, vectors: np.ndarray, chunk_ids: list) -> None:
        """Append a just-saved document to the warm index of its model instead of dropping the index (a
        rebuild re-reads every vector from SQLite). Drops it when the document does not fit the budget."""
        with self._lock:
            self._fts_synced = False        # re-verified (cheaply) before the next keyword search or write
            index = self._indexes.get(model)
            if index is None:
                return
            block = np.asarray(vectors, dtype=np.float32)
            dtype = index.matrix.dtype
            if (index.size and block.shape[1] != index.dim) or len(chunk_ids) != block.shape[0] \
                    or (index.size + block.shape[0]) * block.shape[1] * dtype.itemsize > _index_budget_bytes():
                self._indexes.pop(model, None)
                return
            if dtype == np.int8:
                block = np.clip(np.rint(block * _INT8_SCALE), -127, 127)
            doc_table = list(index.doc_table) + [doc_id]
            self._indexes[model] = _SearchIndex(
                np.concatenate([index.chunk_ids, np.asarray(chunk_ids, dtype=np.int64)]),
                np.concatenate([index.doc_codes, np.full(block.shape[0], len(doc_table) - 1, dtype=np.int32)]),
                doc_table,
                np.concatenate([index.matrix, block.astype(dtype)]) if index.size else block.astype(dtype))

    def _shrink_index(self, doc_id: str) -> None:
        """Remove a deleted document's rows from every warm index."""
        with self._lock:
            self._fts_synced = False
            for model, index in list(self._indexes.items()):
                if doc_id not in index.doc_table:
                    continue
                keep = index.doc_codes != index.doc_table.index(doc_id)
                self._indexes[model] = _SearchIndex(index.chunk_ids[keep], index.doc_codes[keep], index.doc_table,
                                                    index.matrix[keep])

    # ---- keyword index (FTS5, derived from rag_chunks.text) -------------

    @property
    def keyword_available(self) -> bool:
        return self._fts_ok

    def _write_fts_state(self, conn) -> None:
        row = conn.execute("SELECT COUNT(*) AS n, COALESCE(MAX(id), 0) AS m FROM rag_chunks").fetchone()
        for key, value in (("chunks", row["n"]), ("max_id", row["m"])):
            conn.execute("INSERT INTO rag_fts_state (key, value) VALUES (?, ?) "
                         "ON CONFLICT(key) DO UPDATE SET value=excluded.value", (key, int(value)))

    def _rebuild_keyword_index(self, conn) -> None:
        conn.execute(f"INSERT INTO {keyword.FTS_TABLE} ({keyword.FTS_TABLE}) VALUES ('rebuild')")
        self._write_fts_state(conn)

    def _mark_keyword_stale(self, conn=None) -> None:
        """Forget the sync state so the next use rebuilds the keyword index."""
        self._fts_synced = False
        try:
            if conn is not None:
                conn.execute("DELETE FROM rag_fts_state")
            else:
                with self._conn() as own:
                    own.execute("DELETE FROM rag_fts_state")
        except sqlite3.Error as exc:
            log.warning("Could not reset the keyword index state: %s", exc)

    def _fts_apply(self, conn, statements) -> bool:
        """Run FTS statements inside the caller's transaction, then record the new sync state.

        An FTS failure never fails the write: the state row is dropped so the next use rebuilds, and
        False is returned so the caller does not write a fresh state row over the failure.
        """
        try:
            for sql, params in statements:
                conn.execute(sql, params)
            self._write_fts_state(conn)
            return True
        except sqlite3.Error as exc:
            log.warning("Keyword index update failed, it will be rebuilt: %s", exc)
            self._mark_keyword_stale(conn)
            return False

    def _ensure_keyword_index(self) -> bool:
        """True when the keyword index exists and matches rag_chunks; rebuilds it when it does not.

        The table is derived data. Its state row (chunk count, highest chunk id) is compared with the
        chunks table, so rows written or deleted by a build without FTS support are detected.
        """
        with self._lock:
            if not self._fts_ok:
                return False
            if self._fts_synced:
                return True
            try:
                with self._conn() as conn:
                    conn.execute(_FTS_SCHEMA)
                    conn.execute(_FTS_STATE_SCHEMA)
                    now = conn.execute("SELECT COUNT(*) AS n, COALESCE(MAX(id), 0) AS m FROM rag_chunks").fetchone()
                    state = {r["key"]: r["value"] for r in conn.execute("SELECT key, value FROM rag_fts_state")}
                    if state.get("chunks") != now["n"] or state.get("max_id") != now["m"]:
                        self._rebuild_keyword_index(conn)
            except sqlite3.Error as exc:
                log.warning("Keyword index rebuild failed, using vector search only: %s", exc)
                self._fts_ok = False
                return False
            self._fts_synced = True
            return True

    @staticmethod
    def _doc_filter(allowed: Optional[set], params: list) -> str:
        """SQL restricting ``c.doc_id`` to ``allowed`` (appends its parameters); '' when unrestricted."""
        if allowed is None:
            return ""
        if len(allowed) <= 500:
            params += sorted(allowed)
            return f" AND c.doc_id IN ({','.join('?' * len(allowed))})"
        params.append(json.dumps(sorted(allowed)))
        return " AND c.doc_id IN (SELECT value FROM json_each(?))"

    def _cjk_ids(self, terms: list, model: str, allowed: Optional[set], n: int) -> list:
        """Chunk ids containing the most of these character pairs, best first. A scan with ``instr``: the
        FTS5 tokenizer cannot split unspaced scripts, so substring matching is done on the stored text."""
        hits = " + ".join("(instr(c.text, ?) > 0)" for _ in terms)
        params: list = list(terms) + [model]
        sql = (f"SELECT c.id, ({hits}) AS hits FROM rag_chunks c JOIN rag_documents d ON d.id = c.doc_id "
               "WHERE d.model = ?" + self._doc_filter(allowed, params) + " AND hits > 0 ORDER BY hits DESC, c.id LIMIT ?")
        params.append(int(n))
        with self._lock, self._conn() as conn:
            return [int(r["id"]) for r in conn.execute(sql, params)]

    def _keyword_ids(self, expr: str, model: str, allowed: Optional[set], n: int) -> list:
        """Chunk ids matching an FTS5 expression, best bm25 first, for one model and optional documents."""
        with self._lock:
            sql = (f"SELECT c.id FROM {keyword.FTS_TABLE} JOIN rag_chunks c ON c.id = {keyword.FTS_TABLE}.rowid "
                   "JOIN rag_documents d ON d.id = c.doc_id "
                   f"WHERE {keyword.FTS_TABLE} MATCH ? AND d.model = ?")
            params: list = [expr, model]
            sql += self._doc_filter(allowed, params)
            sql += f" ORDER BY {keyword.FTS_TABLE}.rank LIMIT ?"
            params.append(int(n))
            try:
                with self._conn() as conn:
                    return [int(r["id"]) for r in conn.execute(sql, params)]
            except sqlite3.Error:
                self._mark_keyword_stale()
                raise

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
            dim = self._modal_dim(conn, model)
            if not dim:                                       # every stored vector is damaged
                return _SearchIndex(np.empty(0, np.int64), np.empty(0, np.int32), [], np.empty((0, 0), dtype))
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
                rows = self._readable(rows, dim)
                if not rows:
                    continue
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
            stored = self._modal_dim(conn, model)
            if stored and stored != query.shape[0]:
                raise ValueError(
                    f"Query embedding dimension {query.shape[0]} does not match the index "
                    f"dimension {stored}; the embedding model changed.")
            cursor = conn.execute(
                "SELECT c.id, c.doc_id, c.embedding FROM rag_chunks c JOIN rag_documents d "
                "ON d.id = c.doc_id WHERE d.model = ? ORDER BY c.id", (model,))
            while True:
                rows = cursor.fetchmany(_SCAN_BATCH)
                if not rows:
                    break
                rows = self._readable(rows, query.shape[0])
                if not rows:
                    continue
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
               min_score: float = 0.0, document_ids: Optional[list[str]] = None,
               query_text: str = "", mode: str = "vector") -> list[RagChunk]:
        """Exact cosine search over chunks embedded with ``model`` (vectors are L2-normalized).

        With ``mode="hybrid"`` an FTS5 keyword search over ``query_text`` runs as a second leg and the two
        rankings are merged with Reciprocal Rank Fusion. ``score`` is always the cosine similarity (and
        ``min_score`` a cosine floor), ``fused_score`` the RRF value. If the keyword leg cannot run
        (no FTS5, nothing searchable in the text, an FTS error) the result is the vector-only one.
        """
        if top_k < 1:
            raise ValueError("top_k must be at least 1")
        top_k = min(top_k, MAX_TOP_K)
        if not math.isfinite(min_score):
            raise ValueError("min_score must be a finite number")
        if document_ids is not None and not document_ids:
            return []
        query = np.asarray(query_vec, dtype=np.float32).reshape(-1)
        wanted = _candidate_count(top_k)
        allowed = None if document_ids is None else set(document_ids)
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
                vector_ids = [int(i) for i in index.chunk_ids[rows]]
            else:
                vector_ids = self._scan_candidates(model, query, allowed, wanted)
            keyword_ids = self._keyword_candidates(query_text, model, allowed, wanted) if mode == "hybrid" else None
            ids = list(dict.fromkeys(vector_ids + (keyword_ids or [])))
            if not ids:
                return []
            hydrated = self._readable(self._hydrate(ids), query.shape[0])
            if not hydrated:
                return []
            scores = np.array([float(np.frombuffer(r["embedding"], dtype=np.float32) @ query)
                               for r in hydrated])
            row_ids = np.array([r["id"] for r in hydrated], dtype=np.int64)
        fused: Optional[dict] = None
        if keyword_ids is not None:
            cosine = {int(i): float(s) for i, s in zip(row_ids, scores)}
            vector_ranked = sorted((i for i in vector_ids if i in cosine), key=lambda i: (-cosine[i], i))
            fused = keyword.rrf_fuse([vector_ranked, [i for i in keyword_ids if i in cosine]])
            literals = keyword.literal_pieces(query_text)
            hits = ({int(row_ids[p]) for p, row in enumerate(hydrated) if any(piece in row["text"] for piece in literals)}
                    if literals else set())
            # A chunk holding an identifier from the query verbatim outranks every chunk that does not.
            boost = 2.0 / (keyword.RRF_K + 1)
            order = sorted(range(len(hydrated)),
                           key=lambda p: (-(fused.get(int(row_ids[p]), 0.0) + (boost if int(row_ids[p]) in hits else 0.0)),
                                          -scores[p], row_ids[p]))
        else:
            order = [int(p) for p in np.lexsort((row_ids, -scores))]      # best score first, ties by chunk id
        in_vector, in_keyword = set(vector_ids), set(keyword_ids or [])
        results: list[RagChunk] = []
        for pos in order:
            score = float(scores[pos])
            if min_score > 0 and score < min_score:
                continue
            row = hydrated[pos]
            chunk_id = int(row["id"])
            if fused is None:
                match, fused_score = "vector", None
            else:
                match = "both" if chunk_id in in_vector and chunk_id in in_keyword else (
                    "keyword" if chunk_id in in_keyword else "vector")
                fused_score = round(fused[chunk_id], 6)
            results.append(RagChunk(
                document_id=row["doc_id"], document_name=row["doc_name"],
                index=int(row["chunk_index"]), text=row["text"], tokens=int(row["tokens"]),
                score=round(score, 4), match=match, fused_score=fused_score))
            if len(results) >= top_k:
                break
        return results

    def _keyword_candidates(self, query_text: str, model: str, allowed: Optional[set], n: int) -> Optional[list]:
        """Keyword-ranked chunk ids, or None when the keyword leg does not run (then search is vector-only).

        Spaced text goes through the FTS5 index. Unspaced scripts (Chinese, Japanese) cannot be tokenized
        by it, so their character pairs are matched as substrings; when a query has both, the two rankings
        are merged with Reciprocal Rank Fusion."""
        fts_ids: Optional[list] = None
        expr = keyword.match_expression(query_text)
        if expr is not None and self._ensure_keyword_index():
            try:
                fts_ids = self._keyword_ids(expr, model, allowed, n)
            except sqlite3.Error as exc:
                log.warning("Keyword search failed, using vector search only: %s", exc)
        terms = keyword.cjk_terms(query_text)
        cjk_ids = self._cjk_ids(terms, model, allowed, n) if terms else None
        if cjk_ids is None:
            return fts_ids
        if fts_ids is None:
            return cjk_ids
        fused = keyword.rrf_fuse([fts_ids, cjk_ids])
        return sorted(fused, key=lambda i: (-fused[i], i))[:n]

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
