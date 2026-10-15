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
        self._lock = threading.RLock()
        self._users: dict[str, int] = {}
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
            with open(path, "r", encoding="utf-8-sig") as f:
                content = f.read()
        except UnicodeDecodeError as exc:
            raise ValueError("Dataset file must be UTF-8 encoded text "
                             "(JSONL/CSV/plain text)") from exc
        return self.ingest_content(name=name or os.path.splitext(os.path.basename(path))[0],
                                   content=content, fmt=fmt, source=path)

    def ingest_content(self, name: str, content: str, fmt: str = "text",
                       source: str = "") -> DatasetRecord:
        content = content.removeprefix("\ufeff")
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

    def acquire(self, dataset_id: str) -> DatasetRecord:
        with self._lock:
            rec = self.get(dataset_id)
            if rec is None:
                raise ValueError(f"Unknown dataset '{dataset_id}'")
            self._users[dataset_id] = self._users.get(dataset_id, 0) + 1
            return rec

    def release(self, dataset_id: str) -> None:
        with self._lock:
            count = self._users.get(dataset_id, 0)
            if count <= 1:
                self._users.pop(dataset_id, None)
            else:
                self._users[dataset_id] = count - 1

    def delete(self, dataset_id: str) -> bool:
        with self._lock:
            if self._users.get(dataset_id):
                raise RuntimeError("Dataset is in use by a queued or running training job")
            rec = self.get(dataset_id)
            with self._conn() as conn:
                cur = conn.execute("DELETE FROM datasets WHERE id = ?", (dataset_id,))
            if cur.rowcount > 0 and rec and rec.path and os.path.isfile(rec.path):
                os.remove(rec.path)
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
        if not obj["messages"] or any(not isinstance(m, dict) or m.get("role") not in
                ("system", "user", "assistant") or not isinstance(m.get("content"), str)
                for m in obj["messages"]):
            raise ValueError(f"Row {row_num}: invalid chat messages")
        return {"messages": obj["messages"]}
    if "instruction" in obj and "output" in obj:
        if not all(isinstance(obj.get(k, ""), str) for k in ("instruction", "input", "output")):
            raise ValueError(f"Row {row_num}: instruction fields must be strings")
        prompt = "\n".join(s for s in (obj["instruction"].strip(), obj.get("input", "").strip()) if s)
        if not prompt or not obj["output"].strip():
            raise ValueError(f"Row {row_num}: instruction and output are required")
        return {"messages": [{"role": "user", "content": prompt},
                             {"role": "assistant", "content": obj["output"]}]}
    if "text" in obj:
        if not isinstance(obj["text"], str):
            raise ValueError(f"Row {row_num}: text must be a string")
        return {"text": obj["text"]} if obj["text"].strip() else None
    if "prompt" in obj and "completion" in obj:
        if not all(isinstance(obj[k], str) for k in ("prompt", "completion")):
            raise ValueError(f"Row {row_num}: prompt and completion must be strings")
        text = f"{obj['prompt']}\n{obj['completion']}".strip()
        return {"text": text} if text else None
    for key in ("input", "instruction", "content"):
        if key in obj:
            if not isinstance(obj[key], str) or not obj[key].strip():
                raise ValueError(f"Row {row_num}: {key} must be a nonempty string")
            return {"text": obj[key]}
    return None


def _normalize_jsonl(content: str) -> list[dict]:
    samples = []
    for i, line in enumerate(content.splitlines(), 1):
        line = line.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
        except json.JSONDecodeError as exc:
            raise ValueError(f"Row {i}: invalid JSON: {exc.msg}") from exc
        s = _normalize_sample(obj, i)
        if s is not None:
            samples.append(s)
        else:
            raise ValueError(f"Row {i}: unsupported or empty training sample")
    return samples


# Explicit per-field limit in characters; parser failures are user input errors.
CSV_FIELD_LIMIT = 128 * 1024
csv.field_size_limit(CSV_FIELD_LIMIT)


def _normalize_csv(content: str) -> list[dict]:
    samples = []
    reader = csv.DictReader(io.StringIO(content), strict=True)
    try:
        cols = reader.fieldnames
        if not cols:
            return samples
        for row_num, row in enumerate(reader, 2):
            if None in row or any(value is None for value in row.values()):
                raise ValueError(f"Row {row_num}: missing or extra CSV fields")
            if any(key in cols for key in ("text", "instruction", "prompt", "content", "input")):
                sample = _normalize_sample(row, row_num)
            else:
                text = next((row[c] for c in cols if row[c].strip()), "")
                sample = {"text": text} if text else None
            if sample is None:
                raise ValueError(f"Row {row_num}: unsupported or empty training sample")
            samples.append(sample)
    except csv.Error as exc:
        raise ValueError(f"Row {reader.line_num}: invalid CSV ({exc}); maximum field size is {CSV_FIELD_LIMIT} characters") from exc
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
