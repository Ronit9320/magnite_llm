from __future__ import annotations

import math
import os
import threading
import time
import uuid
from typing import Optional

from backend.adapters.store import AdapterStore
from backend.training.datasets import DatasetStore
from backend.training.schemas import TrainingConfig, TrainingJob
from backend.training.trainer import run_training


def _now() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


class TrainingJobManager:
    """Owns training jobs: queuing, live status, cancellation, history.

    Jobs live in memory for the lifetime of the backend (persistence is a
    Phase 9 concern). Each job is executed on its own daemon thread so the
    HTTP event loop stays responsive."""

    def __init__(self, data_dir: str, adapter_store: AdapterStore,
                 dataset_store: DatasetStore):
        self._data_dir = data_dir
        self._adapter_store = adapter_store
        self._datasets = dataset_store
        self._adapter_dir = os.path.join(data_dir, "adapters")
        os.makedirs(self._adapter_dir, exist_ok=True)
        self._lock = threading.Lock()
        self._jobs: dict[str, TrainingJob] = {}
        self._cancel: dict[str, threading.Event] = {}

    def _validate(self, cfg: TrainingConfig) -> None:
        dataset = self._datasets.get(cfg.dataset_id)
        if dataset is None:
            raise ValueError(f"Unknown dataset '{cfg.dataset_id}' — ingest a dataset first")
        if not cfg.base_model.strip():
            raise ValueError("base_model is required")
        if cfg.quantization not in ("none", "4bit", "8bit"):
            raise ValueError(f"Unsupported quantization '{cfg.quantization}'")
        if not cfg.rank or cfg.rank < 1:
            raise ValueError("rank must be >= 1")

    def start(self, cfg: TrainingConfig) -> TrainingJob:
        with self._lock:
            self._validate(cfg)

            dataset = self._datasets.get(cfg.dataset_id)
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
            self._jobs[job_id] = job
            event = threading.Event()
            self._cancel[job_id] = event

            def _run() -> None:
                run_training(job, job_cfg, dataset.path, self._adapter_dir,
                             self._adapter_store, event)

            thread = threading.Thread(
                target=_run, name=f"train-{job_id}", daemon=True
            )
            thread.start()
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
        job = self._jobs.get(job_id)
        if event is None or job is None:
            return False
        if job.status in ("queued", "running"):
            event.set()
            if job.status == "queued":
                job.status = "cancelled"
                job.message = "Cancelled before start"
                job.finished_at = _now()
            return True
        return False