from __future__ import annotations

import json
import os
import sqlite3
import threading
import time
from typing import Optional

from backend.models.schemas import ModelRecord

_SCHEMA = """
CREATE TABLE IF NOT EXISTS models (
    id            TEXT PRIMARY KEY,
    name          TEXT NOT NULL,
    source        TEXT NOT NULL,
    ref           TEXT NOT NULL,
    path          TEXT NOT NULL,
    architecture  TEXT NOT NULL DEFAULT '',
    size_mb       REAL NOT NULL DEFAULT 0,
    license       TEXT NOT NULL DEFAULT '',
    meta          TEXT NOT NULL DEFAULT '{}',
    downloaded_at TEXT NOT NULL DEFAULT '',
    status        TEXT NOT NULL DEFAULT 'ready'
);
"""


class ModelStore:
    """SQLite-backed registry of local models. Thread-safe."""

    def __init__(self, db_path: str):
        self._db_path = db_path
        os.makedirs(os.path.dirname(db_path), exist_ok=True) if os.path.dirname(db_path) else None
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

    def upsert(self, rec: ModelRecord) -> None:
        with self._lock, self._conn() as conn:
            conn.execute(
                """
                INSERT INTO models (id, name, source, ref, path, architecture,
                                    size_mb, license, meta, downloaded_at, status)
                VALUES (:id, :name, :source, :ref, :path, :architecture,
                        :size_mb, :license, :meta, :downloaded_at, :status)
                ON CONFLICT(id) DO UPDATE SET
                    name=excluded.name, source=excluded.source, ref=excluded.ref,
                    path=excluded.path, architecture=excluded.architecture,
                    size_mb=excluded.size_mb, license=excluded.license,
                    meta=excluded.meta, downloaded_at=excluded.downloaded_at,
                    status=excluded.status
                """,
                {
                    "id": rec.id,
                    "name": rec.name,
                    "source": rec.source,
                    "ref": rec.ref,
                    "path": rec.path,
                    "architecture": rec.architecture,
                    "size_mb": rec.size_mb,
                    "license": rec.license,
                    "meta": json.dumps(rec.meta),
                    "downloaded_at": rec.downloaded_at,
                    "status": rec.status,
                },
            )
            conn.commit()

    def get(self, model_id: str) -> Optional[ModelRecord]:
        with self._lock, self._conn() as conn:
            row = conn.execute("SELECT * FROM models WHERE id = ?", (model_id,)).fetchone()
        return self._from_row(row) if row else None

    def list(self, source: Optional[str] = None, q: str = "") -> list[ModelRecord]:
        sql = "SELECT * FROM models"
        params: list = []
        clauses: list[str] = []
        if source:
            clauses.append("source = ?")
            params.append(source)
        if q:
            clauses.append("(name LIKE ? OR ref LIKE ?)")
            params += [f"%{q}%", f"%{q}%"]
        if clauses:
            sql += " WHERE " + " AND ".join(clauses)
        sql += " ORDER BY downloaded_at DESC"
        with self._lock, self._conn() as conn:
            rows = conn.execute(sql, params).fetchall()
        return [self._from_row(r) for r in rows]

    def delete(self, model_id: str) -> bool:
        with self._lock, self._conn() as conn:
            cur = conn.execute("DELETE FROM models WHERE id = ?", (model_id,))
            conn.commit()
            return cur.rowcount > 0

    @staticmethod
    def _from_row(row: sqlite3.Row) -> ModelRecord:
        return ModelRecord(
            id=row["id"],
            name=row["name"],
            source=row["source"],
            ref=row["ref"],
            path=row["path"],
            architecture=row["architecture"],
            size_mb=row["size_mb"],
            license=row["license"],
            meta=json.loads(row["meta"] or "{}"),
            downloaded_at=row["downloaded_at"],
            status=row["status"],
        )