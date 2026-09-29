from __future__ import annotations

import os
import shutil
import zipfile
import json
import re
import stat
import tempfile
import threading
from pathlib import Path, PurePosixPath

from fastapi import APIRouter, HTTPException
from fastapi.responses import FileResponse
from pydantic import BaseModel, ValidationError

from backend.adapters.schemas import AdapterRecord
from backend.adapters.store import AdapterStore
from backend.inference.supervisor import get_supervisor
from backend.paths import app_home

router = APIRouter(prefix="/adapters", tags=["adapters"])

# Assigned at app startup (see server.py).
_store: AdapterStore | None = None
_import_lock = threading.Lock()


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
def list_adapters(q: str = "", base_model: str = ""):
    recs = _get_store().list()
    if q:
        recs = [r for r in recs if q.lower() in r.name.lower()]
    if base_model:
        recs = [r for r in recs if r.base_model == base_model]
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
    # Serialize duplicate checks and registration to avoid overwriting an import.
    with _import_lock:
        return _import_adapter(body)


def _import_adapter(body: AdapterImportRequest):
    """Ingest a Magnite adapter export zip and register the adapter."""
    src = (body.path or "").strip()
    if not src or not os.path.isfile(src):
        raise HTTPException(
            status_code=400, detail="'path' must point to an existing export zip"
        )
    dest = None
    try:
        with zipfile.ZipFile(src) as zf:
            if "record.json" not in zf.namelist():
                raise HTTPException(
                    status_code=400,
                    detail="Not a Magnite adapter export (missing record.json)",
                )
            if zf.getinfo("record.json").file_size > 1024 * 1024:
                raise ValueError("Adapter metadata exceeds 1 MiB")
            rec = AdapterRecord.model_validate_json(zf.read("record.json"))
            if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_:\-]{0,127}", rec.id):
                raise ValueError("Invalid adapter id")
            if _get_store().get(rec.id) is not None:
                raise HTTPException(status_code=409, detail=f"Adapter '{rec.id}' already exists")
            members = []
            total = 0
            seen = set()
            for entry in zf.infolist():
                name = entry.filename
                path = PurePosixPath(name)
                if (path.is_absolute() or ".." in path.parts or "\\" in name
                        or ":" in name or stat.S_ISLNK(entry.external_attr >> 16)):
                    raise ValueError("Unsafe archive member")
                if name in seen:
                    raise ValueError("Duplicate archive member")
                seen.add(name)
                total += entry.file_size
                if total > 8 * 1024**3 or len(seen) > 10000:
                    raise ValueError("Adapter archive exceeds import limits")
                if name.startswith("weights/") and not entry.is_dir():
                    members.append(entry)
            names = {e.filename for e in members}
            if "weights/adapter_config.json" not in names or not names.intersection(
                {"weights/adapter_model.safetensors", "weights/adapter_model.bin"}
            ):
                raise ValueError("Archive must contain PEFT configuration and adapter weights")
            if zf.getinfo("weights/adapter_config.json").file_size > 1024 * 1024:
                raise ValueError("Adapter configuration exceeds 1 MiB")
            config = json.loads(zf.read("weights/adapter_config.json"))
            if not isinstance(config, dict) or not config.get("peft_type"):
                raise ValueError("Invalid adapter configuration")
            if any(e.file_size == 0 for e in members if e.filename in
                   ("weights/adapter_model.safetensors", "weights/adapter_model.bin")):
                raise ValueError("Adapter weights are empty")
            dest = Path(tempfile.mkdtemp(prefix="adapter-", dir=_imports_dir()))
            for entry in members:
                target = (dest / entry.filename[len("weights/"):]).resolve()
                if not target.is_relative_to(dest.resolve()):
                    raise ValueError("Unsafe archive destination")
                target.parent.mkdir(parents=True, exist_ok=True)
                with zf.open(entry) as fsrc, target.open("xb") as fdest:
                    shutil.copyfileobj(fsrc, fdest)
            rec.path = str(dest)
            rec.status = "ready"
            _get_store().upsert(rec)
            return rec
    except (zipfile.BadZipFile, ValidationError, ValueError, KeyError, OSError, RuntimeError) as exc:
        if dest is not None:
            shutil.rmtree(dest, ignore_errors=True)
        raise HTTPException(status_code=400, detail=f"Invalid adapter archive: {exc}") from exc


@router.get("/{adapter_id}", response_model=AdapterRecord)
def get_adapter(adapter_id: str):
    rec = _get_store().get(adapter_id)
    if rec is None:
        raise HTTPException(status_code=404, detail=f"No adapter '{adapter_id}'")
    return rec


def _remove_local(rec: AdapterRecord) -> None:
    if rec.path and os.path.isdir(rec.path):
        path = Path(rec.path).resolve()
        root = (Path(app_home()) / "adapters").resolve()
        if path == root or not path.is_relative_to(root):
            raise HTTPException(status_code=400, detail="Adapter path is outside the managed store")
        if path in (root / "imports", root / "exports"):
            raise HTTPException(status_code=400, detail="Refusing to remove a shared adapter directory")
        shutil.rmtree(path)


@router.delete("/{adapter_id}", status_code=204)
def delete_adapter(adapter_id: str):
    store = _get_store()
    rec = store.get(adapter_id)
    if rec is None:
        raise HTTPException(status_code=404, detail=f"No adapter '{adapter_id}'")
    sup = get_supervisor()
    try:
        with sup.operation():
            if sup.active_adapter is not None and sup.active_adapter.id == adapter_id:
                sup._unload()
            _remove_local(rec)
            store.delete(adapter_id)
    except RuntimeError as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from exc
