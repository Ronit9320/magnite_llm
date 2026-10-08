from __future__ import annotations

import json
import logging
import os
from pathlib import Path
import threading
import time
import uuid
from typing import Optional

import httpx
from huggingface_hub import HfApi, snapshot_download, scan_cache_dir
from huggingface_hub.constants import HF_HUB_CACHE
from huggingface_hub.errors import CacheNotFound
from tqdm import tqdm

from backend.models.schemas import HubSearchResult, ModelRecord, PullJob, model_id
from backend.models.store import ModelStore

OLLAMA_HOST = os.environ.get("MAGNITE_OLLAMA_URL", "http://127.0.0.1:11434")
logger = logging.getLogger(__name__)


class _ProgressTqdm(tqdm):
    """tqdm subclass that mirrors progress into a shared dict without
    printing its own progress bar."""

    def __init__(self, *args, progress_key, **kwargs):
        # Silence visual output: write to a null sink while still tracking
        # (self.n advances regardless of where the bar would render).
        kwargs.pop("file", None)
        null = open(os.devnull, "w")
        super().__init__(*args, file=null, **kwargs)
        self._progress_key = progress_key

    def update(self, n: float = 1) -> Optional[bool]:
        ret = super().update(n)
        state = self._progress_key
        state["current"] = int(self.n)
        state["total"] = int(self.total or 0)
        return ret


class ModelRegistry:
    """Coordinates HF/Ollama downloads with the local SQLite store."""

    def __init__(self, store: ModelStore, hf_cache: Optional[str] = None):
        self._store = store
        self._api = HfApi()
        self._hf_cache = hf_cache if hf_cache is not None else HF_HUB_CACHE
        self._sync_lock = threading.RLock()
        self._jobs: dict[str, PullJob] = {}

    # ---- Job management ------------------------------------------------

    def _create_job(self, ref: str, source: str) -> str:
        job_id = uuid.uuid4().hex[:12]
        self._jobs[job_id] = PullJob(
            id=job_id, ref=ref, source=source, status="queued",
            progress=0.0, message="Queued",
        )
        return job_id

    def job_status(self, job_id: str) -> Optional[PullJob]:
        job = self._jobs.get(job_id)
        if job is None:
            return None
        # Re-read so callers get the latest progress.
        return job.model_copy()

    # ---- HuggingFace Hub ----------------------------------------------

    def search_hub(self, query: str, limit: int = 20) -> list[HubSearchResult]:
        results = []
        for info in self._api.list_models(search=query, limit=limit):
            card = info.cardData or {}
            results.append(
                HubSearchResult(
                    id=model_id("huggingface", info.id),
                    repo_id=info.id,
                    downloads=getattr(info, "downloads", 0) or 0,
                    likes=getattr(info, "likes", 0) or 0,
                    gated=bool(getattr(info, "gated", False)),
                    license=card.get("license", "") or "",
                    tags=list(getattr(info, "tags", []) or []),
                )
            )
        return results

    def _hf_snapshot_path(self, repo_id: str, job_id: str) -> str:
        progress_key = self._jobs[job_id].__dict__
        def tqdm_class(*args, **kwargs):
            return _ProgressTqdm(*args, progress_key=progress_key, **kwargs)
        path = snapshot_download(
            repo_id=repo_id,
            cache_dir=self._hf_cache,
            tqdm_class=tqdm_class,
            local_files_only=False,
        )
        return path

    def _inspect_hf(self, path: str) -> tuple[str, float]:
        """Return (architecture, approx size in MB)."""
        architecture = ""
        config_path = os.path.join(path, "config.json")
        try:
            with open(config_path, "r", encoding="utf-8") as f:
                cfg = json.load(f)
            archs = cfg.get("architectures", []) if isinstance(cfg, dict) else []
            if archs:
                architecture = archs[0] if isinstance(archs, list) else str(archs)
        except (OSError, json.JSONDecodeError):
            pass

        total = 0.0
        for root, _dirs, files in os.walk(path):
            for name in files:
                fpath = os.path.join(root, name)
                try:
                    total += os.path.getsize(fpath)
                except OSError:
                    pass
        return architecture, round(total / (1024 * 1024), 1)

    @staticmethod
    def _has_model_weights(path: Path) -> bool:
        """Recognize local base weights without downloading or importing ML.

        A Hub snapshot can contain only a tokenizer, an adapter, or one shard.
        This is a file-presence check, not validation of model compatibility.
        """
        if any(p.is_file() and p.stat().st_size > 0 for p in path.rglob("*.gguf")):
            return True
        if not (path / "config.json").is_file():
            return False
        for name in ("model.safetensors", "pytorch_model.bin"):
            weight = path / name
            if weight.is_file() and weight.stat().st_size > 0:
                return True
        for name in ("model.safetensors.index.json", "pytorch_model.bin.index.json"):
            try:
                index = json.loads((path / name).read_text(encoding="utf-8"))
                shards = set(index["weight_map"].values())
                if shards and all(
                    not Path(shard).is_absolute() and ".." not in Path(shard).parts
                    for shard in shards
                ) and all((path / shard).is_file() and (path / shard).stat().st_size > 0
                          for shard in shards):
                    return True
            except (OSError, ValueError, KeyError, TypeError, AttributeError):
                continue
        return False

    def _record_hf_models(self) -> None:
        """Discover externally downloaded models; never mutate the shared cache."""
        if not Path(self._hf_cache).expanduser().exists():
            return
        try:
            cache = scan_cache_dir(self._hf_cache)
        except (OSError, ValueError, CacheNotFound) as exc:
            logger.warning("Cannot scan Hugging Face cache: %s", exc)
            return
        for warning in cache.warnings:
            logger.warning("Hugging Face cache: %s", warning)
        for repo in sorted(cache.repos, key=lambda r: r.repo_id):
            if repo.repo_type != "model":
                continue
            identifier = model_id("huggingface", repo.repo_id)
            existing = self._store.get(identifier)
            try:
                revisions = [r for r in repo.revisions if self._has_model_weights(r.snapshot_path)]
                if not revisions:
                    continue
                # Keep an explicitly registered revision; otherwise prefer main.
                revision = max(revisions, key=lambda r: (
                    bool(existing and Path(existing.path) == r.snapshot_path),
                    "main" in r.refs, r.last_modified, r.commit_hash,
                ))
                architecture, size_mb = self._inspect_hf(str(revision.snapshot_path))
                rec = ModelRecord(
                    id=identifier, name=existing.name if existing else repo.repo_id.rsplit("/", 1)[-1],
                    source="huggingface", ref=repo.repo_id, path=str(revision.snapshot_path),
                    architecture=architecture, size_mb=size_mb,
                    license=existing.license if existing else "",
                    meta={**(existing.meta if existing else {}), "cache_revision": revision.commit_hash},
                    downloaded_at=(existing.downloaded_at if existing else "") or
                        time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(revision.last_modified)),
                    status="ready",
                )
                if existing != rec:
                    self._store.upsert(rec)
            except (OSError, ValueError) as exc:
                # A concurrent external download/deletion must not break the list.
                logger.warning("Cannot inspect cached model %s: %s", repo.repo_id, exc)

    def pull_hf(self, repo_id: str) -> str:
        job_id = self._create_job(repo_id, "huggingface")
        thread = threading.Thread(
            target=self._run_hf_pull, args=(repo_id, job_id), daemon=True
        )
        thread.start()
        return job_id

    def _run_hf_pull(self, repo_id: str, job_id: str) -> None:
        job = self._jobs[job_id]
        try:
            job.status = "running"
            job.message = "Downloading from HuggingFace Hub"
            path = self._hf_snapshot_path(repo_id, job_id)

            job.message = "Inspecting model"
            architecture, size_mb = self._inspect_hf(path)

            info = self._api.model_info(repo_id, files_metadata=False)
            card = info.cardData or {}
            license_val = card.get("license", "") or ""
            name = repo_id.rsplit("/", 1)[-1]

            rec = ModelRecord(
                id=model_id("huggingface", repo_id),
                name=name,
                source="huggingface",
                ref=repo_id,
                path=path,
                architecture=architecture,
                size_mb=size_mb,
                license=license_val,
                meta={"downloads": getattr(info, "downloads", 0) or 0,
                      "likes": getattr(info, "likes", 0) or 0,
                      "gated": bool(getattr(info, "gated", False)),
                      "tags": list(getattr(info, "tags", []) or [])},
                downloaded_at=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                status="ready",
            )
            self._store.upsert(rec)
            job.status = "done"
            job.progress = 100.0
            job.message = f"Ready — {rec.name} ({size_mb} MB)"
        except Exception as exc:  # noqa: BLE001
            job.status = "error"
            job.error = str(exc)
            job.message = f"Failed: {exc}"

    def _remove_hf(self, rec: ModelRecord) -> None:
        """Best-effort removal of the snapshot cache dir since HF has no CLI delete."""
        from pathlib import Path
        from huggingface_hub import scan_cache_dir

        snapshot = Path(rec.path).absolute()
        if not snapshot.exists():
            return
        if snapshot.parent.name != "snapshots":
            raise ValueError("Model path is not a managed Hugging Face snapshot")
        cache = scan_cache_dir(snapshot.parent.parent.parent)
        for repo in cache.repos:
            for revision in repo.revisions:
                if revision.snapshot_path == snapshot:
                    cache.delete_revisions(revision.commit_hash).execute()
                    return
        raise ValueError("Cannot safely identify cached model revision; files were not removed")

    # ---- Ollama --------------------------------------------------------

    def ollama_available(self) -> bool:
        try:
            r = httpx.get(f"{OLLAMA_HOST}/api/tags", timeout=2.0)
            return r.status_code == 200
        except httpx.HTTPError:
            return False

    def list_ollama(self) -> list[ModelRecord]:
        try:
            r = httpx.get(f"{OLLAMA_HOST}/api/tags", timeout=5.0)
            r.raise_for_status()
        except httpx.HTTPError:
            return []
        records = []
        for m in r.json().get("models", []):
            name = m.get("name", "")
            if not name:
                continue
            details = m.get("details") or {}
            rec = ModelRecord(
                id=model_id("ollama", name),
                name=name.split(":")[0],
                source="ollama",
                ref=name,
                path=name,  # Ollama owns the storage; store the name handle
                architecture=details.get("family", ""),
                size_mb=round((m.get("size") or 0) / (1024 * 1024), 1),
                license=details.get("license", ""),
                meta={"parameter_size": details.get("parameter_size", ""),
                      "quantization": details.get("quantization_level", "")},
                downloaded_at=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                status="ready",
            )
            records.append(rec)
        return records

    def _record_ollama_models(self) -> None:
        for rec in self.list_ollama():
            self._store.upsert(rec)

    def pull_ollama(self, name: str) -> str:
        job_id = self._create_job(name, "ollama")
        thread = threading.Thread(
            target=self._run_ollama_pull, args=(name, job_id), daemon=True
        )
        thread.start()
        return job_id

    def _run_ollama_pull(self, model_name: str, job_id: str) -> None:
        job = self._jobs[job_id]
        try:
            job.status = "running"
            job.message = f"Requesting {model_name} from Ollama"
            # Push a placeholder so C++ gets a running status immediately.
            with httpx.stream(
                "POST", f"{OLLAMA_HOST}/api/pull",
                json={"name": model_name, "stream": True},
                timeout=httpx.Timeout(10.0, connect=5.0),
            ) as resp:
                resp.raise_for_status()
                for line in resp.iter_lines():
                    if not line:
                        continue
                    try:
                        payload = json.loads(line)
                    except json.JSONDecodeError:
                        continue
                    if "status" in payload:
                        job.message = payload["status"]
                    total = payload.get("total") or 0
                    completed = payload.get("completed") or 0
                    if total:
                        job.progress = min(100.0, round(100.0 * completed / total, 1))
            self._record_ollama_models()
            job.status = "done"
            job.progress = 100.0
            job.message = f"Ready — {model_name}"
        except Exception as exc:  # noqa: BLE001
            job.status = "error"
            job.error = str(exc)
            job.message = f"Failed: {exc}"

    # ---- registry helpers ------------------------------------------------

    def list_models(self, source: Optional[str] = None) -> list[ModelRecord]:
        return self._store.list(source=source)

    def get_model(self, identifier: str) -> Optional[ModelRecord]:
        return self._store.get(identifier)

    def list_synced(self, source: Optional[str] = None, q: str = "") -> list[ModelRecord]:
        """Refresh local HF snapshots and Ollama before applying list filters."""
        if source in (None, "huggingface"):
            with self._sync_lock:
                self._record_hf_models()
        if source in (None, "ollama"):
            try:
                self._record_ollama_models()
            except Exception:  # noqa: BLE001 -- ollama may just be offline
                pass
        return self._store.list(source=source, q=q)

    def delete_model(self, model_id: str) -> bool:
        with self._sync_lock:
            return self._delete_model(model_id)

    def _delete_model(self, model_id: str) -> bool:
        rec = self._store.get(model_id)
        if rec is None:
            return False
        if rec.source == "huggingface":
            self._remove_hf(rec)
        elif rec.source == "ollama":
            response = httpx.request("DELETE", f"{OLLAMA_HOST}/api/delete", json={"model": rec.ref}, timeout=30.0)
            if response.status_code != 404:
                response.raise_for_status()
        return self._store.delete(model_id)
