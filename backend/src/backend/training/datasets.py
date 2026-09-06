from __future__ import annotations

import csv
import io
import json
import os
import sqlite3
import threading
import time
import uuid
from typing import Optional

from backend.training.schemas import DatasetRecord

_SCHEMA = """
CREATE TABLE IF NOT EXISTS datasets (
    id         TEXT PRIMARY KEY,
    name       TEXT NOT NULL,
    format     TEXT NOT NULL,
    path       TEXT NOT NULL,
    rows       INTEGER NOT NULL DEFAULT 0,
    size_kb    REAL NOT NULL DEFAULT 0,
    source     TEXT NOT NULL DEFAULT '',
    created_at TEXT NOT NULL DEFAULT ''
);
"""


def _now() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


class DatasetStore:
    """Ingests JSONL/CSV/plain-text datasets and normalizes them to JSONL."""

    def __init__(self, data_dir: str, db_path: str):
        self._data_dir = data_dir
        self._db_path = db_path
        os.makedirs(data_dir, exist_ok=True)
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

    # ---- ingestion ------------------------------------------------------

    def ingest_path(self, path: str, name: str = "", fmt: str = "") -> DatasetRecord:
        path = os.path.abspath(path)
        if not os.path.isfile(path):
            raise ValueError(f"Dataset file not found: {path}")
        fmt = fmt or _detect_format(path)
        try:
            with open(path, "r", encoding="utf-8") as f:
                content = f.read()
        except UnicodeDecodeError as exc:
            raise ValueError("Dataset file must be UTF-8 encoded text "
                             "(JSONL/CSV/plain text)") from exc
        return self.ingest_content(name=name or os.path.splitext(os.path.basename(path))[0],
                                   content=content, fmt=fmt, source=path)

    def ingest_content(self, name: str, content: str, fmt: str = "text",
                       source: str = "") -> DatasetRecord:
        fmt = fmt.lower()
        if fmt not in ("jsonl", "csv", "text"):
            raise ValueError(f"Unsupported dataset format '{fmt}'")

        # Normalize to a list of {"text": ...} / {"messages": [...]} dicts.
        if fmt == "jsonl":
            samples = _normalize_jsonl(content)
        elif fmt == "csv":
            samples = _normalize_csv(content)
        else:
            samples = _normalize_text(content)

        if not samples:
            raise ValueError("Dataset contains no usable samples")

        dataset_id = uuid.uuid4().hex[:12]
        out_path = os.path.join(self._data_dir, f"{dataset_id}.jsonl")
        with open(out_path, "w", encoding="utf-8") as f:
            for s in samples:
                f.write(json.dumps(s, ensure_ascii=False) + "\n")

        rec = DatasetRecord(
            id=dataset_id,
            name=name or f"dataset-{dataset_id[:6]}",
            format=fmt,
            path=out_path,
            rows=len(samples),
            size_kb=round(os.path.getsize(out_path) / 1024.0, 1),
            source=source,
            created_at=_now(),
        )
        self._upsert(rec)
        return rec

    # ---- queries --------------------------------------------------------

    def get(self, dataset_id: str) -> Optional[DatasetRecord]:
        with self._lock, self._conn() as conn:
            row = conn.execute(
                "SELECT * FROM datasets WHERE id = ?", (dataset_id,)
            ).fetchone()
        return self._from_row(row) if row else None

    def list(self) -> list[DatasetRecord]:
        with self._lock, self._conn() as conn:
            rows = conn.execute(
                "SELECT * FROM datasets ORDER BY created_at DESC"
            ).fetchall()
        return [self._from_row(r) for r in rows]

    def delete(self, dataset_id: str) -> bool:
        rec = self.get(dataset_id)
        with self._lock, self._conn() as conn:
            cur = conn.execute("DELETE FROM datasets WHERE id = ?", (dataset_id,))
            conn.commit()
        if cur.rowcount > 0 and rec and rec.path and os.path.isfile(rec.path):
            try:
                os.remove(rec.path)
            except OSError:
                pass
        return cur.rowcount > 0

    # ---- internals ------------------------------------------------------

    def _upsert(self, rec: DatasetRecord) -> None:
        with self._lock, self._conn() as conn:
            conn.execute(
                """
                INSERT INTO datasets (id, name, format, path, rows, size_kb,
                                      source, created_at)
                VALUES (:id, :name, :format, :path, :rows, :size_kb,
                        :source, :created_at)
                ON CONFLICT(id) DO UPDATE SET
                    name=excluded.name, format=excluded.format, path=excluded.path,
                    rows=excluded.rows, size_kb=excluded.size_kb,
                    source=excluded.source, created_at=excluded.created_at
                """,
                {
                    "id": rec.id,
                    "name": rec.name,
                    "format": rec.format,
                    "path": rec.path,
                    "rows": rec.rows,
                    "size_kb": rec.size_kb,
                    "source": rec.source,
                    "created_at": rec.created_at,
                },
            )
            conn.commit()

    @staticmethod
    def _from_row(row: sqlite3.Row) -> DatasetRecord:
        return DatasetRecord(
            id=row["id"],
            name=row["name"],
            format=row["format"],
            path=row["path"],
            rows=row["rows"],
            size_kb=row["size_kb"],
            source=row["source"],
            created_at=row["created_at"],
        )


def _detect_format(path: str) -> str:
    ext = os.path.splitext(path)[1].lower()
    if ext in (".jsonl", ".ndjson", ".json"):
        return "jsonl"
    if ext == ".csv":
        return "csv"
    return "text"


def _normalize_sample(obj: dict, row_num: int) -> Optional[dict]:
    if not isinstance(obj, dict):
        return None
    if "messages" in obj and isinstance(obj["messages"], list):
        return {"messages": obj["messages"]}
    if "text" in obj and str(obj["text"]).strip():
        return {"text": str(obj["text"])}
    if "prompt" in obj and "completion" in obj:
        text = f"{obj['prompt']}\n{obj['completion']}".strip()
        return {"text": text} if text else None
    for key in ("input", "instruction", "content"):
        if key in obj and str(obj[key]).strip():
            return {"text": str(obj[key])}
    return None


def _normalize_jsonl(content: str) -> list[dict]:
    samples = []
    for i, line in enumerate(content.splitlines()):
        line = line.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            continue
        s = _normalize_sample(obj, i)
        if s is not None:
            samples.append(s)
    return samples


def _normalize_csv(content: str) -> list[dict]:
    samples = []
    reader = csv.DictReader(io.StringIO(content))
    if not reader.fieldnames:
        return samples
    cols = reader.fieldnames
    text_col = "text" if "text" in cols else None
    for row in reader:
        if text_col and str(row.get(text_col, "")).strip():
            samples.append({"text": str(row[text_col])})
            continue
        if "prompt" in cols and "completion" in cols:
            text = f"{row.get('prompt', '')}\n{row.get('completion', '')}".strip()
            if text:
                samples.append({"text": text})
                continue
        # Fall back to the first column that has content.
        for c in cols:
            if str(row.get(c, "")).strip():
                samples.append({"text": str(row[c])})
                break
    return samples


def _normalize_text(content: str) -> list[dict]:
    samples = []
    for para in content.replace("\r\n", "\n").split("\n\n"):
        para = " ".join(para.split())
        if not para:
            continue
        # Split overly long paragraphs into sentence-sized chunks.
        chunk = ""
        for sentence in _sentences(para):
            candidate = f"{chunk} {sentence}".strip()
            if len(candidate) > 900 and chunk:
                samples.append({"text": chunk})
                chunk = sentence
            else:
                chunk = candidate
        if chunk:
            samples.append({"text": chunk})
    return samples


def _sentences(paragraph: str) -> list[str]:
    import re

    return [s.strip() for s in re.split(r"(?<=[.!?])\s+", paragraph) if s.strip()]