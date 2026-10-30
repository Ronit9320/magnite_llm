from __future__ import annotations

import os

from fastapi import APIRouter, HTTPException, Query

from backend.paths import app_home
from backend.training.datasets import DatasetStore
from backend.training.jobs import TrainingJobManager
from backend.training.trainer import quantization_support
from backend.training.schemas import (
    DatasetIngestRequest,
    DatasetRecord,
    TrainingConfig,
    TrainingJob,
)

training_router = APIRouter(prefix="/training", tags=["training"])
datasets_router = APIRouter(prefix="/datasets", tags=["datasets"])

# Assigned at app startup (see server.py).
_manager: TrainingJobManager | None = None
_datasets: DatasetStore | None = None


def _get_manager() -> TrainingJobManager:
    if _manager is None:
        raise HTTPException(status_code=503, detail="Training not initialized")
    return _manager


def _get_datasets() -> DatasetStore:
    if _datasets is None:
        raise HTTPException(status_code=503, detail="Training not initialized")
    return _datasets


def _training_setup() -> None:
    """Called once from server startup."""
    global _manager, _datasets

    home = app_home()
    from backend.adapters.store import AdapterStore

    adapter_store = AdapterStore(os.path.join(home, "adapters.db"))
    _datasets = DatasetStore(
        data_dir=os.path.join(home, "datasets"),
        db_path=os.path.join(home, "datasets.db"),
    )
    _manager = TrainingJobManager(home, adapter_store, _datasets)


# ---- training endpoints -------------------------------------------------


@training_router.post("/start", response_model=TrainingJob, status_code=202)
def start_training(cfg: TrainingConfig):
    try:
        return _get_manager().start(cfg)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except RuntimeError as exc:
        raise HTTPException(status_code=503, detail=str(exc)) from exc


@training_router.get("/capabilities")
def training_capabilities():
    supported, reason = quantization_support()
    return {"quantization": supported, "quantization_detail": reason}


@training_router.get("/status/{job_id}", response_model=TrainingJob)
def training_status(job_id: str):
    job = _get_manager().get(job_id)
    if job is None:
        raise HTTPException(status_code=404, detail=f"No training job '{job_id}'")
    return job


@training_router.get("/jobs", response_model=list[TrainingJob])
def training_jobs():
    return _get_manager().list()


@training_router.post("/cancel/{job_id}", response_model=TrainingJob)
def cancel_training(job_id: str):
    manager = _get_manager()
    if not manager.cancel(job_id):
        raise HTTPException(status_code=404, detail=f"No cancellable job '{job_id}'")
    job = manager.get(job_id)
    if job is None:
        raise HTTPException(status_code=404, detail=f"No training job '{job_id}'")
    return job


# ---- dataset endpoints --------------------------------------------------


@datasets_router.post("/ingest", response_model=DatasetRecord, status_code=201)
def ingest_dataset(body: DatasetIngestRequest):
    store = _get_datasets()
    try:
        if body.path:
            return store.ingest_path(body.path, name=body.name, fmt=body.format)
        if body.content:
            if not body.name.strip():
                raise ValueError("'name' is required when sending raw content")
            return store.ingest_content(body.name.strip(), body.content,
                                        fmt=body.format or "text", source="upload")
        raise ValueError("Provide either 'path' or 'content'")
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc


@datasets_router.get("", response_model=list[DatasetRecord])
def list_datasets(q: str = Query(default="", description="Name filter")):
    recs = _get_datasets().list()
    if q:
        recs = [r for r in recs if q.lower() in r.name.lower()]
    return recs


@datasets_router.get("/{dataset_id}", response_model=DatasetRecord)
def get_dataset(dataset_id: str):
    rec = _get_datasets().get(dataset_id)
    if rec is None:
        raise HTTPException(status_code=404, detail=f"No dataset '{dataset_id}'")
    return rec


@datasets_router.delete("/{dataset_id}", status_code=204)
def delete_dataset(dataset_id: str):
    try:
        deleted = _get_datasets().delete(dataset_id)
    except RuntimeError as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from exc
    if not deleted:
        raise HTTPException(status_code=404, detail=f"No dataset '{dataset_id}'")