from __future__ import annotations

import os
import httpx

from fastapi import APIRouter, HTTPException, Query

from backend.models.registry import ModelRegistry
from backend.models.schemas import HubSearchResult, ModelRecord, PullJob, PullRequest

router = APIRouter(prefix="/models", tags=["models"])

# Assigned at app startup (see server.py). Kept module-level so it is shared
# between the registry and any future routers that need it.
_registry: ModelRegistry | None = None


def _get_registry() -> ModelRegistry:
    if _registry is None:
        raise HTTPException(status_code=503, detail="Registry not initialized")
    return _registry


def _registry_setup() -> None:
    """Called once from server startup."""
    global _registry

    app_dir = os.environ.get("MAGNITE_HOME")
    if not app_dir:
        app_dir = os.path.join(os.path.expanduser("~"), ".magnite")
        if os.name == "nt":
            app_dir = os.path.join(os.environ.get("APPDATA") or app_dir, "Magnite")
    db_path = os.path.join(app_dir, "models.db")
    hf_cache = os.environ.get("HF_HOME") or None

    from backend.models.store import ModelStore

    store = ModelStore(db_path)
    _registry = ModelRegistry(store, hf_cache=hf_cache)


@router.get("", response_model=list[ModelRecord])
def list_models(
    source: str | None = Query(default=None, description="Filter by source"),
    q: str = Query(default="", description="Name/ref substring filter"),
):
    return _get_registry().list_synced(source=source, q=q)


@router.get("/search", response_model=list[HubSearchResult])
def search_hub(
    q: str = Query(..., description="Search query"),
    limit: int = Query(default=20, ge=1, le=100),
):
    if not q.strip():
        return []
    return _get_registry().search_hub(q.strip(), limit=limit)


@router.get("/{model_id}", response_model=ModelRecord)
def get_model(model_id: str):
    rec = _get_registry().list_synced()
    for m in rec:
        if m.id == model_id:
            return m
    raise HTTPException(status_code=404, detail=f"Model '{model_id}' not found")


@router.post("/pull", response_model=PullJob, status_code=202)
def pull_model(body: PullRequest):
    reg = _get_registry()
    if body.source not in ("huggingface", "ollama"):
        raise HTTPException(status_code=400, detail=f"Unknown source '{body.source}'")
    if body.source == "ollama":
        job_id = reg.pull_ollama(body.ref)
    else:
        job_id = reg.pull_hf(body.ref)
    return reg.job_status(job_id) or HTTPException(status_code=500, detail="Job lost")


@router.get("/pull/{job_id}", response_model=PullJob)
def pull_status(job_id: str):
    job = _get_registry().job_status(job_id)
    if job is None:
        raise HTTPException(status_code=404, detail=f"No pull job '{job_id}'")
    return job


@router.delete("/{model_id}", status_code=204)
def delete_model(model_id: str):
    from backend.inference.supervisor import get_supervisor
    from backend.routes.training import _manager
    try:
        registry = _get_registry()
        record = registry.get_model(model_id)
        if record is None:
            raise HTTPException(status_code=404, detail=f"Model '{model_id}' not found")
        references = {model_id, record.path, record.ref}
        sup = get_supervisor()
        with sup.operation():
            if sup._engine is not None and sup.model_ref in references:
                raise HTTPException(status_code=409, detail="Unload this model before deleting it")
            if _manager and any(j.status in ("queued", "running") and j.base_model in references
                                for j in _manager.list()):
                raise HTTPException(status_code=409, detail="Cancel training jobs using this model before deleting it")
            registry.delete_model(model_id)
    except RuntimeError as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from exc
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except (httpx.HTTPError, OSError) as exc:
        raise HTTPException(status_code=502, detail=f"Deletion failed; model remains registered: {exc}") from exc
