from __future__ import annotations

import os
import shutil

from fastapi import APIRouter, HTTPException

from backend.adapters.schemas import AdapterRecord
from backend.adapters.store import AdapterStore
from backend.paths import app_home

router = APIRouter(prefix="/adapters", tags=["adapters"])

# Assigned at app startup (see server.py).
_store: AdapterStore | None = None


def _get_store() -> AdapterStore:
    if _store is None:
        raise HTTPException(status_code=503, detail="Adapter store not initialized")
    return _store


def _adapter_setup() -> None:
    global _store
    _store = AdapterStore(os.path.join(app_home(), "adapters.db"))


@router.get("", response_model=list[AdapterRecord])
def list_adapters(q: str = ""):
    recs = _get_store().list()
    if q:
        recs = [r for r in recs if q.lower() in r.name.lower()]
    return recs


@router.get("/{adapter_id}", response_model=AdapterRecord)
def get_adapter(adapter_id: str):
    rec = _get_store().get(adapter_id)
    if rec is None:
        raise HTTPException(status_code=404, detail=f"No adapter '{adapter_id}'")
    return rec


def _remove_local(rec: AdapterRecord) -> None:
    if rec.path and os.path.isdir(rec.path):
        try:
            shutil.rmtree(rec.path, ignore_errors=True)
        except OSError:
            pass


@router.delete("/{adapter_id}", status_code=204)
def delete_adapter(adapter_id: str):
    store = _get_store()
    rec = store.get(adapter_id)
    if rec is None:
        raise HTTPException(status_code=404, detail=f"No adapter '{adapter_id}'")
    _remove_local(rec)
    store.delete(adapter_id)