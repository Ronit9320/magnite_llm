from __future__ import annotations

import queue
import os
import threading
import time
import uuid
from typing import Optional

from backend.database import connection
from backend.adapters.store import AdapterStore
from backend.training.datasets import DatasetStore
from backend.training.schemas import TrainingConfig, TrainingJob
from backend.training.trainer import quantization_support, run_training, TrainingCancellation


def _now() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


class TrainingJobManager:
    """Serialized training queue with durable terminal history and restart recovery."""

    def __init__(self, data_dir: str, adapter_store: AdapterStore,
                 dataset_store: DatasetStore):
        self._data_dir = data_dir
        self._adapter_store = adapter_store
        self._datasets = dataset_store
        self._adapter_dir = os.path.join(data_dir, "adapters")
        os.makedirs(self._adapter_dir, exist_ok=True)
        self._lock = threading.Lock()
        self._persist_lock = threading.Lock()
        self._jobs: dict[str, TrainingJob] = {}
        self._cancel: dict[str, threading.Event] = {}

        self._db = os.path.join(data_dir, "training_jobs.db")
        with connection(self._db) as conn:
            conn.execute("CREATE TABLE IF NOT EXISTS jobs (id TEXT PRIMARY KEY, record TEXT NOT NULL)")
            for (record,) in conn.execute("SELECT record FROM jobs"):
                job = TrainingJob.model_validate_json(record)
                if job.status in ("queued", "running"):
                    job.status = "error"
                    job.error = "Training interrupted by backend shutdown. Submit a new job to retry."
                    job.message = job.error
                    job.finished_at = _now()
                self._jobs[job.id] = job
        for job in self._jobs.values():
            self._persist(job)
        self._queue = queue.Queue()
        self._closing = False
        self._worker = threading.Thread(target=self._run_queue, daemon=True, name="training-queue")
        self._worker.start()

    def _persist(self, job):
        # Serialize the snapshot and commit together. Otherwise a shutdown
        # snapshot of "running" can wait on SQLite and overwrite the worker's
        # newer "done" record after that worker has committed.
        with self._persist_lock, connection(self._db) as conn:
            conn.execute("INSERT OR REPLACE INTO jobs VALUES (?, ?)", (job.id, job.model_dump_json()))

    def _run_queue(self):
        while True:
            item = self._queue.get()
            if item is None:
                return
            job, cfg, dataset_path, event = item
            try:
                if event.is_set():
                    continue
                run_training(job, cfg, dataset_path, self._adapter_dir, self._adapter_store, event)
            except Exception as exc:
                job.status = "error"
                job.error = str(exc)
                job.message = str(exc)
                job.finished_at = _now()
            finally:
                self._datasets.release(cfg.dataset_id)
                self._persist(job)

    def shutdown(self):
        with self._lock:
            self._closing = True
            for job_id, event in self._cancel.items():
                event.set()
                job = self._jobs[job_id]
                if job.status == "queued":
                    job.status = "cancelled"
                    job.message = "Cancelled during shutdown"
                    job.finished_at = _now()
                self._persist(job)
            self._queue.put(None)
        self._worker.join(timeout=2)

    def _validate(self, cfg: TrainingConfig) -> None:
        dataset = self._datasets.get(cfg.dataset_id)
        if dataset is None:
            raise ValueError(f"Unknown dataset '{cfg.dataset_id}' — ingest a dataset first")
        if not cfg.base_model.strip():
            raise ValueError("base_model is required")
        if cfg.quantization not in ("none", "4bit", "8bit"):
            raise ValueError(f"Unsupported quantization '{cfg.quantization}'")
        if cfg.quantization != "none":
            supported, reason = quantization_support()
            if not supported:
                raise ValueError(reason)
        if not cfg.rank or cfg.rank < 1:
            raise ValueError("rank must be >= 1")

    def start(self, cfg: TrainingConfig) -> TrainingJob:
        with self._lock:
            if self._closing:
                raise RuntimeError("Training service is shutting down")
            self._validate(cfg)

            job_id = uuid.uuid4().hex[:12]
            name = (cfg.name or "").strip() or f"adapter-{job_id[:6]}"
            job_cfg = cfg.model_copy(update={"name": name})
            job = TrainingJob(
                id=job_id,
                name=name,
                status="queued",
                base_model=cfg.base_model,
                dataset_id=cfg.dataset_id,
                config=job_cfg.model_dump(),
                progress=0.0,
                message="Queued",
                created_at=_now(),
            )
            dataset = self._datasets.acquire(cfg.dataset_id)
            self._jobs[job_id] = job
            event = TrainingCancellation()
            self._cancel[job_id] = event

            try:
                self._persist(job)
                self._queue.put((job, job_cfg, dataset.path, event))
            except Exception:
                self._datasets.release(cfg.dataset_id)
                self._jobs.pop(job_id, None)
                self._cancel.pop(job_id, None)
                raise
            return job.model_copy()

    def get(self, job_id: str) -> Optional[TrainingJob]:
        job = self._jobs.get(job_id)
        return job.model_copy() if job is not None else None

    def list(self) -> list[TrainingJob]:
        jobs = list(self._jobs.values())
        jobs.sort(key=lambda j: j.created_at, reverse=True)
        return [j.model_copy() for j in jobs]

    def cancel(self, job_id: str) -> bool:
        event = self._cancel.get(job_id)
        if event is None:
            return False
        with event.publication_lock:
            return self._cancel_job(job_id)

    def _cancel_job(self, job_id: str) -> bool:
        event = self._cancel.get(job_id)
        job = self._jobs.get(job_id)
        if event is None or job is None:
            return False
        if job.status in ("queued", "running"):
            event.set()
            if job.status == "queued":
                job.status = "cancelled"
                job.message = "Cancelled before start"
                job.finished_at = _now()
            self._persist(job)
            return True
        return False
