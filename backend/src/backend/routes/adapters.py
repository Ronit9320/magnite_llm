from __future__ import annotations

import os
import shutil
import zipfile

from fastapi import APIRouter, HTTPException
from fastapi.responses import FileResponse
from pydantic import BaseModel

from backend.adapters.schemas import AdapterRecord
from backend.adapters.store import AdapterStore
from backend.inference.supervisor import get_supervisor
from backend.paths import app_home

router = APIRouter(prefix="/adapters", tags=["adapters"])

# Assigned at app startup (see server.py).
_store: AdapterStore | None = None


class AdapterImportRequest(BaseModel):
    path: str = ""


class AdapterExportResult(BaseModel):
    id: str
    download_path: str
    size_bytes: int


def _get_store() -> AdapterStore:
    if _store is None:
        raise HTTPException(status_code=503, detail="Adapter store not initialized")
    return _store


def _adapter_setup() -> AdapterStore:
    global _store
    _store = AdapterStore(os.path.join(app_home(), "adapters.db"))
    return _store


def _exports_dir() -> str:
    exports = os.path.join(app_home(), "adapters", "exports")
    os.makedirs(exports, exist_ok=True)
    return exports


def _imports_dir() -> str:
    imports = os.path.join(app_home(), "adapters", "imports")
    os.makedirs(imports, exist_ok=True)
    return imports


@router.get("", response_model=list[AdapterRecord])
def list_adapters(q: str = ""):
    recs = _get_store().list()
    if q:
        recs = [r for r in recs if q.lower() in r.name.lower()]
    return recs


@router.post("/export/{adapter_id}", response_model=AdapterExportResult)
def export_adapter(adapter_id: str):
    """Package an adapter record + weights into a zip under adapters/exports."""
    rec = _get_store().get(adapter_id)
    if rec is None:
        raise HTTPException(status_code=404, detail=f"No adapter '{adapter_id}'")
    safe = rec.id.replace(":", "-").replace("/", "_")
    zip_path = os.path.join(_exports_dir(), f"{safe}.zip")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as zf:
        zf.writestr("record.json", rec.model_dump_json(indent=2))
        if rec.path and os.path.isdir(rec.path):
            for root, _dirs, files in os.walk(rec.path):
                for name in files:
                    full = os.path.join(root, name)
                    rel = os.path.relpath(full, rec.path)
                    zf.write(full, os.path.join("weights", rel))
    return AdapterExportResult(
        id=rec.id,
        download_path=f"/adapters/export/{safe}.zip",
        size_bytes=os.path.getsize(zip_path),
    )


@router.get("/export/{filename}")
def export_download(filename: str):
    """Stream a previously exported adapter zip."""
    safe = os.path.basename(filename)
    path = os.path.join(_exports_dir(), safe)
    if not os.path.isfile(path):
        raise HTTPException(status_code=404, detail="No such export")
    return FileResponse(path, media_type="application/zip", filename=safe)


@router.post("/import", response_model=AdapterRecord, status_code=201)
def import_adapter(body: AdapterImportRequest):
    """Ingest a Magnite adapter export zip and register the adapter."""
    src = (body.path or "").strip()
    if not src or not os.path.isfile(src):
        raise HTTPException(
            status_code=400, detail="'path' must point to an existing export zip"
        )
    try:
        with zipfile.ZipFile(src) as zf:
            if "record.json" not in zf.namelist():
                raise HTTPException(
                    status_code=400,
                    detail="Not a Magnite adapter export (missing record.json)",
                )
            rec = AdapterRecord.model_validate_json(zf.read("record.json"))
    except zipfile.BadZipFile:
        raise HTTPException(status_code=400, detail="Not a valid zip archive")
    if _get_store().get(rec.id) is not None:
        raise HTTPException(status_code=409, detail=f"Adapter '{rec.id}' already exists")
    dest = os.path.join(_imports_dir(), rec.id.replace(":", "-").replace("/", "_"))
    os.makedirs(dest, exist_ok=True)
    with zipfile.ZipFile(src) as zf:
        for name in zf.namelist():
            if name.startswith("weights/"):
                target = os.path.join(dest, name[len("weights/"):])
                os.makedirs(os.path.dirname(target), exist_ok=True)
                with zf.open(name) as fsrc, open(target, "wb") as fdest:
                    shutil.copyfileobj(fsrc, fdest)
    rec.path = dest
    _get_store().upsert(rec)
    return rec


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
    sup = get_supervisor()
    if sup.active_adapter is not None and sup.active_adapter.id == adapter_id:
        sup.deactivate_adapter()
    _remove_local(rec)
    store.delete(adapter_id)