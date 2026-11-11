"""Local, read-only settings information. Never import an ML runtime to inspect it."""
from __future__ import annotations

import importlib.metadata
import os
from pathlib import Path
import platform
import re
import stat
import time

from fastapi import APIRouter, HTTPException
from backend._version import RELEASE_VERSION
from backend.paths import app_home

router = APIRouter(prefix="/system", tags=["system"])


def package_version(name: str) -> str | None:
    try:
        return importlib.metadata.version(name)
    except importlib.metadata.PackageNotFoundError:
        return None


@router.get("/info")
def system_info():
    from backend.training.telemetry import device_telemetry
    return {"version": RELEASE_VERSION, "system": platform.system(),
            "architecture": platform.machine(), "python": platform.python_version(),
            "engines": {name: package_version(distribution) for name, distribution in
                        (("llama.cpp", "llama-cpp-python"), ("vLLM", "vllm"),
                         ("torch", "torch"), ("PEFT", "peft"))},
            "device": device_telemetry()}


def directory_size(root: Path, *, exclude: Path | None = None,
                   budget_seconds: float = 1.0, max_entries: int = 30000) -> dict:
    """Bounded scan; never follow links or present an incomplete scan as exact."""
    total = entries = 0
    complete = True
    deadline = time.monotonic() + budget_seconds
    pending = [root]
    while pending:
        if time.monotonic() > deadline:
            return {"bytes": total, "complete": False}
        directory = pending.pop()
        if exclude is not None and directory == exclude:
            continue
        try:
            with os.scandir(directory) as children:
                for entry in children:
                    entries += 1
                    if entries > max_entries or time.monotonic() > deadline:
                        return {"bytes": total, "complete": False}
                    if entry.is_symlink():
                        continue
                    if entry.is_dir(follow_symlinks=False):
                        pending.append(Path(entry.path))
                    elif entry.is_file(follow_symlinks=False):
                        total += entry.stat(follow_symlinks=False).st_size
        except FileNotFoundError:
            # A missing root is an empty location; disappearance mid-scan is partial.
            if directory != root:
                complete = False
        except OSError:
            complete = False
    return {"bytes": total, "complete": complete}


def _existing_ancestor(path: Path) -> Path:
    while not path.exists() and path != path.parent:
        path = path.parent
    return path


def _volume_id(path: Path) -> int | None:
    try:
        return os.stat(_existing_ancestor(path)).st_dev
    except OSError:
        return None


def _disk_usage(path: Path) -> dict:
    import shutil
    try:
        usage = shutil.disk_usage(_existing_ancestor(path))
        return {"total": usage.total, "free": usage.free}
    except OSError:
        return {"total": None, "free": None}


@router.get("/storage")
def storage_info():
    from huggingface_hub.constants import HF_HUB_CACHE
    home, cache = Path(app_home()).resolve(), Path(HF_HUB_CACHE).resolve()
    # Count every location once, including user configurations with nested roots.
    home_size = directory_size(home, exclude=cache if cache != home else None)
    cache_size = ({"bytes": 0, "complete": True} if home == cache else
                  directory_size(cache, exclude=home))
    disk = _disk_usage(home)
    home_volume, cache_volume = _volume_id(home), _volume_id(cache)
    return {"data_directory": str(home), "model_cache": str(cache),
            "workspace": home_size, "models": cache_size,
            "combined_locations": home == cache,
            # Capacity describes the data volume only; the cache may live on another disk.
            "cache_on_data_volume": home == cache or (home_volume is not None and home_volume == cache_volume),
            "disk_total_bytes": disk["total"], "free_bytes": disk["free"]}


def redact_log(text: str) -> str:
    text = re.sub(r"(?i)(bearer\s+)\S+", r"\1[redacted]", text)
    text = re.sub(r"(?i)((?:api[_-]?key|token|password|secret)\s*[=:]\s*)[^\s,;]+",
                  r"\1[redacted]", text)
    return text


@router.get("/logs")
def local_logs():
    # No caller-controlled file paths. Keep reads bounded even for large logs.
    path = Path(app_home()) / "vllm.log"
    if path.is_symlink():
        raise HTTPException(status_code=409, detail="Refusing to read a linked log file")
    try:
        descriptor = os.open(path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0))
        with os.fdopen(descriptor, "rb") as file:
            if not stat.S_ISREG(os.fstat(file.fileno()).st_mode):
                raise HTTPException(status_code=409, detail="Runtime log is not a regular file")
            file.seek(0, os.SEEK_END)
            length = file.tell()
            file.seek(max(0, length - 65536))
            text = file.read(65536).decode("utf-8", errors="replace")
    except FileNotFoundError:
        return {"text": "No vLLM runtime log has been created.", "truncated": False}
    except OSError as exc:
        raise HTTPException(status_code=500, detail="Could not read the runtime log") from exc
    return {"text": redact_log(text), "truncated": length > 65536}
