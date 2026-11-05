from __future__ import annotations

import collections
import concurrent.futures
import itertools
import json
import logging
import os
from pathlib import Path
import subprocess
import sys
import threading
import time
import uuid
from typing import Callable, Optional

import httpx
from huggingface_hub import HfApi, scan_cache_dir
from huggingface_hub.constants import HF_HUB_CACHE
from huggingface_hub.errors import CacheNotFound

from backend.models.hubsize import repo_size_bytes
from backend.models.schemas import HubSearchResult, ModelRecord, PullJob, model_id
from backend.models.store import ModelStore

OLLAMA_HOST = os.environ.get("MAGNITE_OLLAMA_URL", "http://127.0.0.1:11434")
logger = logging.getLogger(__name__)


def _default_pull_command(repo_id: str, cache_dir: str) -> list[str]:
    flags = ["-B"]
    if sys.flags.isolated:
        flags.append("-I")
    return [sys.executable, *flags, "-m", "backend.models.pull_worker", repo_id, cache_dir]


class _Cancelled(Exception):
    """Raised inside a pull when the user cancelled it."""


class _PullControl:
    """Cancel flag and worker process for one download."""

    def __init__(self) -> None:
        self.cancel = threading.Event()
        self.lock = threading.Lock()
        self.proc: Optional[subprocess.Popen] = None
        self.thread: Optional[threading.Thread] = None

    def kill(self) -> None:
        with self.lock:
            proc = self.proc
        if proc is not None and proc.poll() is None:
            try:
                proc.kill()
            except OSError:
                pass


class CacheTransaction:
    """What a Hub cache held for one repo before a download started.

    Remove new files in the repo and its lock folder. Shared cache-root blobs
    cannot be attributed to this download: other Hub clients may be using the
    same cache concurrently. Leave those alone, even if created during this job.
    """

    def __init__(self, cache_dir: str, repo_id: str) -> None:
        self.cache = Path(cache_dir).expanduser()
        # No path separators survive, so the folder is always one level below the cache.
        folder = "models--" + repo_id.replace("/", "--").replace("\\", "--")
        self.repo = self.cache / folder
        self.locks_root = self.cache / ".locks"
        self.locks = self.locks_root / folder
        self._before = self._scan()

    def _scan(self) -> set[Path]:
        found: set[Path] = set()
        for top in (self.locks_root,):
            if os.path.lexists(top):
                found.add(top)
        for root in (self.repo, self.locks):
            if not os.path.lexists(root):
                continue
            found.add(root)
            if root.is_dir() and not root.is_symlink():
                for dirpath, dirnames, filenames in os.walk(root, followlinks=False):
                    for name in dirnames + filenames:
                        found.add(Path(dirpath) / name)
        return found

    def rollback(self) -> None:
        created = sorted(self._scan() - self._before, key=lambda p: len(p.parts), reverse=True)
        for path in created:   # deepest first: files empty their folders before the folders go
            try:
                if path.is_symlink() or path.is_file():
                    path.unlink()
                elif path.is_dir():
                    path.rmdir()   # refuses non-empty folders, which is the safe outcome
            except FileNotFoundError:
                continue
            except OSError as exc:
                logger.warning("Cannot remove partial download %s: %s", path, exc)


class ModelRegistry:
    """Coordinates HF/Ollama downloads with the local SQLite store."""

    def __init__(self, store: ModelStore, hf_cache: Optional[str] = None,
                 pull_command: Optional[Callable[[str, str], list[str]]] = None):
        self._store = store
        self._api = HfApi()
        self._hf_cache = hf_cache if hf_cache is not None else HF_HUB_CACHE
        self._sync_lock = threading.RLock()
        self._jobs: dict[str, PullJob] = {}
        self._controls: dict[str, _PullControl] = {}
        self._pull_lock = threading.Lock()
        self._pull_command = pull_command or _default_pull_command
        # Hub repo sizes by lower-cased repo id; only successful lookups are kept.
        self._size_cache: dict[str, int] = {}
        self._size_cache_limit = 2000
        self._size_lock = threading.Lock()

    # ---- Job management ------------------------------------------------

    def has_active_pulls(self) -> bool:
        return any(job.status in ("queued", "running") for job in list(self._jobs.values()))

    def _active_job(self, source: str) -> Optional[PullJob]:
        for job in list(self._jobs.values()):
            if job.source == source and job.status in ("queued", "running"):
                return job
        return None

    def _active_hf_repos(self) -> set[str]:
        return {j.ref.lower() for j in list(self._jobs.values())
                if j.source == "huggingface" and j.status in ("queued", "running")}

    def _create_job(self, ref: str, source: str) -> str:
        job_id = uuid.uuid4().hex[:12]
        self._jobs[job_id] = PullJob(
            id=job_id, ref=ref, source=source, status="queued",
            progress=0.0, message="Queued",
        )
        self._controls[job_id] = _PullControl()
        return job_id

    def _start_pull(self, source: str, ref: str, target) -> str:
        """Start a download thread. One download per source at a time: asking for the
        one already running returns it, and asking for another is refused."""
        with self._pull_lock:
            active = self._active_job(source)
            if active is not None:
                if active.ref == ref:
                    return active.id
                raise RuntimeError(
                    f"Another download is in progress ({active.ref}). Wait for it or cancel it first.")
            job_id = self._create_job(ref, source)
        thread = threading.Thread(target=target, args=(ref, job_id), daemon=True)
        self._controls[job_id].thread = thread
        thread.start()
        return job_id

    def job_status(self, job_id: str) -> Optional[PullJob]:
        job = self._jobs.get(job_id)
        if job is None:
            return None
        # Re-read so callers get the latest progress.
        return job.model_copy()

    def cancel_pull(self, job_id: str) -> Optional[PullJob]:
        """Ask a running download to stop. Returns the job, or None if unknown.

        The download is killed at once; the job turns "cancelled" when its partial
        files have been removed. Cancelling a finished job changes nothing.
        """
        job, control = self._jobs.get(job_id), self._controls.get(job_id)
        if job is None:
            return None
        if control is not None and job.status in ("queued", "running"):
            control.cancel.set()
            job.message = "Cancelling…"
            control.kill()
        return job.model_copy()

    def shutdown(self, timeout: float = 10.0) -> None:
        """Stop every running download (backend exit) and let it clean up."""
        controls = list(self._controls.values())
        for control in controls:
            control.cancel.set()
            control.kill()
        deadline = time.monotonic() + timeout
        for control in controls:
            if control.thread is not None and control.thread is not threading.current_thread():
                control.thread.join(max(0.0, deadline - time.monotonic()))

    # ---- HuggingFace Hub ----------------------------------------------

    def search_hub(self, query: str, limit: int = 20, offset: int = 0) -> list[HubSearchResult]:
        """One page of Hub results, each marked if it is already downloaded.

        The Hub client has no offset, only a lazy iterator, so a page is a slice
        of the first ``offset + limit`` results. The order is the Hub's own, which
        is what keeps consecutive pages from overlapping.
        """
        infos = itertools.islice(
            self._api.list_models(search=query, limit=offset + limit), offset, offset + limit)
        downloaded = self._downloaded_hub_repos()
        results = []
        for info in infos:
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
                    downloaded=info.id.lower() in downloaded,
                )
            )
        return results

    def repo_sizes(self, repo_ids: list[str], timeout: float = 25.0) -> dict[str, Optional[int]]:
        """Exact download size in bytes for each repo, or None where it cannot be found.

        One metadata request per uncached repo, six at a time. A failure (offline,
        gated, slow) is None for that repo only and is not cached, so a retry can
        succeed; whatever is still running when the timeout hits finishes in the
        background and lands in the cache for next time.
        """
        wanted = list(dict.fromkeys(r.strip() for r in repo_ids if r and r.strip()))
        result: dict[str, Optional[int]] = {}
        missing: list[str] = []
        with self._size_lock:
            for repo in wanted:
                cached = self._size_cache.get(repo.lower())
                if cached is None:
                    missing.append(repo)
                else:
                    result[repo] = cached
        if missing:
            pool = concurrent.futures.ThreadPoolExecutor(max_workers=6, thread_name_prefix="hub-size")
            futures = {pool.submit(self._fetch_repo_size, repo): repo for repo in missing}
            done, pending = concurrent.futures.wait(futures, timeout=timeout)
            for future in done:
                result[futures[future]] = future.result()
            for future in pending:
                future.cancel()
                result[futures[future]] = None
            pool.shutdown(wait=False, cancel_futures=True)
        return {repo: result[repo] for repo in wanted}

    def _fetch_repo_size(self, repo: str) -> Optional[int]:
        try:
            size = repo_size_bytes(self._api, repo)
        except Exception as exc:  # noqa: BLE001 -- a missing size must never break search
            logger.debug("No size for %s: %s", repo, exc)
            return None
        if size <= 0:
            return None
        with self._size_lock:
            if len(self._size_cache) >= self._size_cache_limit:
                self._size_cache.pop(next(iter(self._size_cache)))
            self._size_cache[repo.lower()] = size
        return size

    def _downloaded_hub_repos(self) -> set[str]:
        """Lower-cased repo ids of ready Hugging Face models in the local library.

        Uses the same synced view as the Library list, so models downloaded outside
        the app count too. This is a decoration on search results: if the local
        scan fails, results are still returned, just without the marker.
        """
        try:
            return {m.ref.lower() for m in self.list_synced(source="huggingface")
                    if m.status == "ready"} - self._active_hf_repos()
        except Exception as exc:  # noqa: BLE001
            logger.warning("Cannot determine downloaded models for search results: %s", exc)
            return set()

    def _hf_snapshot_path(self, repo_id: str, job_id: str) -> str:
        """Download in a child process and return the snapshot folder.

        Returns "" if the download was cancelled; raises RuntimeError on failure.
        Byte progress is copied onto the job as it arrives.
        """
        job, control = self._jobs[job_id], self._controls[job_id]
        command = self._pull_command(repo_id, self._hf_cache)
        with control.lock:
            if control.cancel.is_set():
                return ""
            try:
                control.proc = subprocess.Popen(
                    command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace", bufsize=1)
            except OSError as exc:
                raise RuntimeError(f"Could not start the download worker: {exc}") from exc
            proc = control.proc

        result_path, error = "", ""
        noise: collections.deque[str] = collections.deque(maxlen=3)
        for line in proc.stdout:
            try:
                message = json.loads(line)
                if not isinstance(message, dict):
                    raise ValueError
            except ValueError:
                if line.strip():
                    noise.append(line.strip())
                continue
            if "total" in message:
                job.bytes_total = max(job.bytes_total, int(message["total"]))
            if "done" in message:
                done = int(message["done"])
                job.bytes_done = max(job.bytes_done, min(done, job.bytes_total) if job.bytes_total else done)
                if job.bytes_total:
                    job.progress = min(99.0, round(100.0 * job.bytes_done / job.bytes_total, 1))
            if "error" in message:
                error = str(message["error"])
            if message.get("result") == "ok":
                result_path = str(message.get("path", ""))
        try:
            code = proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()
            code = proc.wait()
        if control.cancel.is_set():
            return ""
        if error:
            raise RuntimeError(error)
        if code != 0:
            detail = f": {noise[-1]}" if noise else ""
            raise RuntimeError(f"The download worker exited unexpectedly (code {code}){detail}")
        if not result_path or not os.path.isdir(result_path):
            raise RuntimeError("The download finished but produced no model folder")
        return result_path

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
        downloading = self._active_hf_repos()
        for repo in sorted(cache.repos, key=lambda r: r.repo_id):
            if repo.repo_type != "model":
                continue
            if repo.repo_id.lower() in downloading:
                continue   # still arriving: not a usable model yet
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
        return self._start_pull("huggingface", repo_id, self._run_hf_pull)

    def _run_hf_pull(self, repo_id: str, job_id: str) -> None:
        job, control = self._jobs[job_id], self._controls[job_id]
        identifier = model_id("huggingface", repo_id)
        transaction = CacheTransaction(self._hf_cache, repo_id)
        had_record = self._store.get(identifier) is not None
        try:
            job.status = "running"
            job.message = "Downloading from HuggingFace Hub"
            path = self._hf_snapshot_path(repo_id, job_id)
            if control.cancel.is_set() or not path:
                raise _Cancelled()

            job.message = "Inspecting model"
            architecture, size_mb = self._inspect_hf(path)

            info = self._api.model_info(repo_id, files_metadata=False)
            card = info.cardData or {}
            license_val = card.get("license", "") or ""
            name = repo_id.rsplit("/", 1)[-1]

            rec = ModelRecord(
                id=identifier,
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
            if control.cancel.is_set():
                raise _Cancelled()
            self._store.upsert(rec)
            job.status = "done"
            job.progress = 100.0
            job.bytes_done = job.bytes_total = max(job.bytes_total, job.bytes_done)
            job.message = f"Ready — {rec.name} ({size_mb} MB)"
        except _Cancelled:
            self._finish_cancelled(job, transaction, identifier, had_record)
        except Exception as exc:  # noqa: BLE001
            if control.cancel.is_set():
                self._finish_cancelled(job, transaction, identifier, had_record)
            else:
                # Partial files stay: a retry resumes from them.
                job.status = "error"
                job.error = str(exc)
                job.message = f"Failed: {exc}"

    def _finish_cancelled(self, job: PullJob, transaction: CacheTransaction,
                          identifier: str, had_record: bool) -> None:
        transaction.rollback()
        if not had_record:
            self._store.delete(identifier)
        job.status = "cancelled"
        job.error = None
        job.message = "Cancelled. Repository partial files removed; shared cache retained."

    def _remove_hf(self, rec: ModelRecord, *, all_revisions: bool = False) -> None:
        """Remove a model's snapshot from the Hub cache.

        By default only the registered revision goes, and blobs still used by another
        revision stay. Deleting a model from the library passes ``all_revisions`` so no
        older revision is left behind to reappear on the next refresh (the library shows
        one entry per repo). Hugging Face has no CLI delete, so this uses the cache API.
        """
        import dataclasses
        import shutil
        from pathlib import Path
        from huggingface_hub import scan_cache_dir

        # Resolve symlinked ancestors (macOS /var -> /private/var, a symlinked HF_HOME):
        # scan_cache_dir reports resolved snapshot paths.
        snapshot = Path(rec.path).resolve()
        if not snapshot.exists():
            return
        if snapshot.parent.name != "snapshots":
            raise ValueError("Model path is not a managed Hugging Face snapshot")
        repo_folder = snapshot.parent.parent
        cache_root = repo_folder.parent
        cache = scan_cache_dir(cache_root)
        for repo in cache.repos:
            if Path(repo.repo_path).resolve() != repo_folder:
                continue
            if all_revisions:
                hashes = [revision.commit_hash for revision in repo.revisions]
            else:
                hashes = [revision.commit_hash for revision in repo.revisions
                          if Path(revision.snapshot_path).resolve() == snapshot]
            if not hashes:
                break
            # delete_revisions() finds revisions by commit hash across every repo it is
            # given, taking the first match in arbitrary order. Forks and mirrors share
            # commit hashes, so give it only this repo or it may delete the wrong one's files.
            only_this_repo = dataclasses.replace(cache, repos=frozenset({repo}))
            strategy = only_this_repo.delete_revisions(*hashes)
            retained_blobs = {
                Path(file.blob_path).resolve()
                for other in cache.repos if other != repo
                for revision in other.revisions for file in revision.files
            }
            if any(blob.is_relative_to(repo_folder) for blob in retained_blobs):
                raise ValueError("Another cached model references this repository's files; deletion refused")
            strategy = dataclasses.replace(strategy, blobs=frozenset(
                blob for blob in strategy.blobs if Path(blob).resolve() not in retained_blobs))
            strategy.execute()
            if all_revisions:
                # The download leaves one lock file per blob behind; only noise once it is gone.
                shutil.rmtree(cache_root / ".locks" / repo_folder.name, ignore_errors=True)
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
        return self._start_pull("ollama", name, self._run_ollama_pull)

    def _run_ollama_pull(self, model_name: str, job_id: str) -> None:
        job, control = self._jobs[job_id], self._controls[job_id]
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
                    if control.cancel.is_set():
                        break
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
                        job.progress = min(99.0, round(100.0 * completed / total, 1))
                        job.bytes_total, job.bytes_done = int(total), int(completed)
            if control.cancel.is_set():
                # Ollama keeps the layers it already fetched and resumes them next time.
                job.status = "cancelled"
                job.message = "Cancelled."
                return
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
            if rec.ref.lower() in self._active_hf_repos():
                raise RuntimeError("This model is being downloaded. Cancel the download before deleting it.")
            self._remove_hf(rec, all_revisions=True)
        elif rec.source == "ollama":
            response = httpx.request("DELETE", f"{OLLAMA_HOST}/api/delete", json={"model": rec.ref}, timeout=30.0)
            if response.status_code != 404:
                response.raise_for_status()
        return self._store.delete(model_id)
