from __future__ import annotations

import json
import os
import sqlite3
import threading
from typing import Optional

from backend.adapters.schemas import AdapterRecord

_SCHEMA = """
CREATE TABLE IF NOT EXISTS adapters (
    id              TEXT PRIMARY KEY,
    name            TEXT NOT NULL,
    base_model      TEXT NOT NULL,
    architecture    TEXT NOT NULL DEFAULT '',
    rank            INTEGER NOT NULL DEFAULT 0,
    alpha           INTEGER NOT NULL DEFAULT 0,
    target_modules  TEXT NOT NULL DEFAULT '',
    method          TEXT NOT NULL DEFAULT 'lora',
    quantization    TEXT NOT NULL DEFAULT 'none',
    path            TEXT NOT NULL DEFAULT '',
    size_mb         REAL NOT NULL DEFAULT 0,
    training_job_id TEXT NOT NULL DEFAULT '',
    trained_at      TEXT NOT NULL DEFAULT '',
    meta            TEXT NOT NULL DEFAULT '{}',
    status          TEXT NOT NULL DEFAULT 'ready'
);
"""


class AdapterStore:
    """SQLite-backed registry of trained adapters. Thread-safe."""

    def __init__(self, db_path: str):
        self._db_path = db_path
        parent = os.path.dirname(db_path)
        if parent:
            os.makedirs(parent, exist_ok=True)
        self._lock = threading.Lock()
        self._init_db()

    def _init_db(self) -> None:
        with self._lock, sqlite3.connect(self._db_path) as conn:
            conn.execute(_SCHEMA)
            conn.commit()

    def _conn(self) -> sqlite3.Connection:
        conn = sqlite3.connect(self._db_path)
        conn.row_factory = sqlite3.Row
        return conn

    def upsert(self, rec: AdapterRecord) -> None:
        with self._lock, self._conn() as conn:
            conn.execute(
                """
                INSERT INTO adapters (id, name, base_model, architecture, rank,
                                      alpha, target_modules, method, quantization,
                                      path, size_mb, training_job_id, trained_at,
                                      meta, status)
                VALUES (:id, :name, :base_model, :architecture, :rank, :alpha,
                        :target_modules, :method, :quantization, :path, :size_mb,
                        :training_job_id, :trained_at, :meta, :status)
                ON CONFLICT(id) DO UPDATE SET
                    name=excluded.name, base_model=excluded.base_model,
                    architecture=excluded.architecture, rank=excluded.rank,
                    alpha=excluded.alpha, target_modules=excluded.target_modules,
                    method=excluded.method, quantization=excluded.quantization,
                    path=excluded.path, size_mb=excluded.size_mb,
                    training_job_id=excluded.training_job_id,
                    trained_at=excluded.trained_at, meta=excluded.meta,
                    status=excluded.status
                """,
                {
                    "id": rec.id,
                    "name": rec.name,
                    "base_model": rec.base_model,
                    "architecture": rec.architecture,
                    "rank": rec.rank,
                    "alpha": rec.alpha,
                    "target_modules": rec.target_modules,
                    "method": rec.method,
                    "quantization": rec.quantization,
                    "path": rec.path,
                    "size_mb": rec.size_mb,
                    "training_job_id": rec.training_job_id,
                    "trained_at": rec.trained_at,
                    "meta": json.dumps(rec.meta),
                    "status": rec.status,
                },
            )
            conn.commit()

    def get(self, adapter_id: str) -> Optional[AdapterRecord]:
        with self._lock, self._conn() as conn:
            row = conn.execute(
                "SELECT * FROM adapters WHERE id = ?", (adapter_id,)
            ).fetchone()
        return self._from_row(row) if row else None

    def list(self) -> list[AdapterRecord]:
        with self._lock, self._conn() as conn:
            rows = conn.execute(
                "SELECT * FROM adapters ORDER BY trained_at DESC"
            ).fetchall()
        return [self._from_row(r) for r in rows]

    def delete(self, adapter_id: str) -> bool:
        with self._lock, self._conn() as conn:
            cur = conn.execute("DELETE FROM adapters WHERE id = ?", (adapter_id,))
            conn.commit()
            return cur.rowcount > 0

    @staticmethod
    def _from_row(row: sqlite3.Row) -> AdapterRecord:
        return AdapterRecord(
            id=row["id"],
            name=row["name"],
            base_model=row["base_model"],
            architecture=row["architecture"],
            rank=row["rank"],
            alpha=row["alpha"],
            target_modules=row["target_modules"],
            method=row["method"],
            quantization=row["quantization"],
            path=row["path"],
            size_mb=row["size_mb"],
            training_job_id=row["training_job_id"],
            trained_at=row["trained_at"],
            meta=json.loads(row["meta"] or "{}"),
            status=row["status"],
        )