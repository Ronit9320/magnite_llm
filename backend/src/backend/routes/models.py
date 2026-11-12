from __future__ import annotations

import os
from typing import Annotated, Optional

import httpx
from pydantic import StringConstraints

from fastapi import APIRouter, HTTPException, Query, Response

from backend.models.registry import ModelRegistry
from backend.models.schemas import HubSearchResult, ModelRecord, PullJob, PullRequest
from backend.paths import app_home

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

    db_path = os.path.join(app_home(), "models.db")

    from backend.models.store import ModelStore

    store = ModelStore(db_path)
    _registry = ModelRegistry(store)


@router.get("", response_model=list[ModelRecord])
def list_models(
    source: str | None = Query(default=None, description="Filter by source"),
    q: str = Query(default="", description="Name/ref substring filter"),
):
    return _get_registry().list_synced(source=source, q=q)


@router.get("/search", response_model=list[HubSearchResult])
def search_hub(
    q: str = Query(..., description="Search query"),
    limit: int = Query(default=20, ge=1, le=100, description="Page size"),
    offset: int = Query(default=0, ge=0, le=1000, description="Results to skip, for paging"),
):
    if not q.strip():
        return []
    try:
        return _get_registry().search_hub(q.strip(), limit=limit, offset=offset)
    except (httpx.HTTPError, OSError) as exc:  # includes huggingface_hub's HfHubHTTPError
        raise HTTPException(status_code=502, detail=f"Could not reach the Hugging Face Hub: {exc}") from exc


@router.get("/discover", response_model=list[HubSearchResult])
def discover_hub(
    q: str = Query(default="", max_length=200),
    limit: int = Query(default=30, ge=1, le=100),
    offset: int = Query(default=0, ge=0, le=1000),
):
    try:
        return _get_registry().search_hub(q.strip(), limit=limit, offset=offset, discover=True)
    except (httpx.HTTPError, OSError) as exc:
        raise HTTPException(status_code=502, detail=f"Could not reach the Hugging Face Hub: {exc}") from exc


@router.get("/hub-detail")
def hub_detail(repo: str = Query(min_length=1, max_length=200)):
    from huggingface_hub.utils import HFValidationError, validate_repo_id
    try:
        validate_repo_id(repo)
    except HFValidationError as exc:
        raise HTTPException(status_code=400, detail="Invalid Hub repository ID") from exc
    registry = _get_registry()
    try:
        return registry.hub_details(repo)
    except (httpx.HTTPError, OSError) as exc:
        raise HTTPException(status_code=502, detail=f"Could not read model metadata: {exc}") from exc


@router.get("/sizes", response_model=dict[str, Optional[int]])
def hub_sizes(
    repo: list[Annotated[str, StringConstraints(min_length=1, max_length=200)]] = Query(
        ..., min_length=1, max_length=100,
        description="Hub repo ids; repeat the parameter. Returns exact download bytes, or null if unknown."),
):
    """Download sizes for search results. Never fails because the Hub does: a size
    that cannot be found is null."""
    return _get_registry().repo_sizes(repo)


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
    try:
        job_id = reg.pull_ollama(body.ref) if body.source == "ollama" else reg.pull_hf(body.ref)
    except RuntimeError as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from exc
    job = reg.job_status(job_id)
    if job is None:
        raise HTTPException(status_code=500, detail="Job lost")
    return job


@router.get("/pull/{job_id}", response_model=PullJob)
def pull_status(job_id: str):
    job = _get_registry().job_status(job_id)
    if job is None:
        raise HTTPException(status_code=404, detail=f"No pull job '{job_id}'")
    return job


@router.post("/pull/{job_id}/cancel", response_model=PullJob, status_code=202)
def cancel_pull(job_id: str, response: Response):
    """Stop a download and remove its partial files. 202 while it is being cancelled;
    200 with the unchanged job if it had already finished."""
    reg = _get_registry()
    before = reg.job_status(job_id)
    if before is None:
        raise HTTPException(status_code=404, detail=f"No pull job '{job_id}'")
    job = reg.cancel_pull(job_id)
    if before.status not in ("queued", "running"):
        response.status_code = 200
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
