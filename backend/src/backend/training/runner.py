"""Run a training job in a child process and apply what it reports to the job.

The child (`backend.training.train_worker`) follows the downloader's pattern
(`models/pull_worker.py`, supervised from `models/registry.py`): it prints one JSON
object per stdout line and this module applies those events to the job. The backend
registers the adapter itself and ends the child if a cancel does not take effect, so a
training crash (out of memory, a native fault) cannot take chat, RAG or the model
library down with it.
"""
from __future__ import annotations

import collections
import json
import logging
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time

from backend.adapters.store import AdapterStore
from backend.training import trainer as _trainer
from backend.training.schemas import TrainingConfig, TrainingJob
from backend.training.trainer import (
    TrainOutcome,
    _Cancelled,
    adapter_dir_for,
    apply_event,
    mark_cancelled,
    mark_failed,
    publish_adapter,
    require_ml_installed,
)

logger = logging.getLogger(__name__)

# After a cancel is accepted the worker gets this long to stop on its own, then it is killed.
_CANCEL_GRACE_SECONDS = 10.0
_OUT_OF_MEMORY = ("The training process was killed by the system, most likely because the machine ran out "
                  "of memory. Close other apps or choose a smaller model or batch size.")


def _default_train_command(spec_path: str) -> list[str]:
    flags = ["-B"]
    if sys.flags.isolated:
        flags.append("-I")
    return [sys.executable, *flags, "-m", "backend.training.train_worker", spec_path]


def describe_exit(returncode: int) -> str:
    """What a worker that ended without reporting a result should say to the user."""
    if returncode == -9:
        return _OUT_OF_MEMORY
    if returncode < 0:
        return f"The training process crashed (signal {-returncode})."
    if returncode == 0:
        return "The training process finished without reporting a result."
    return f"The training process exited unexpectedly (code {returncode})."


_live: set[subprocess.Popen] = set()
_live_lock = threading.Lock()


def live_workers() -> int:
    with _live_lock:
        return len(_live)


def terminate_all(grace: float = 5.0) -> None:
    """Ask every running training worker to stop, then kill those still alive after `grace` seconds."""
    with _live_lock:
        procs = [proc for proc in _live if proc.poll() is None]
    for proc in procs:
        try:
            proc.stdin.write("cancel\n")
            proc.stdin.flush()
        except (OSError, ValueError):
            pass
    deadline = time.monotonic() + grace
    for proc in procs:
        try:
            proc.wait(timeout=max(0.0, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            try:
                proc.kill()
            except OSError:
                pass
            try:
                proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass


def _watch_for_cancel(proc: subprocess.Popen, cancel: threading.Event, finished: threading.Event) -> None:
    """When the cancel event is set, ask the worker to stop; kill it if it does not."""
    while not finished.is_set():
        if cancel.wait(0.1):
            break
    else:
        return
    try:
        proc.stdin.write("cancel\n")
        proc.stdin.flush()
    except (OSError, ValueError):
        pass
    try:
        proc.wait(timeout=_CANCEL_GRACE_SECONDS)
    except subprocess.TimeoutExpired:
        try:
            proc.kill()
        except OSError:
            pass


def run_training_isolated(job: TrainingJob, cfg: TrainingConfig, dataset_path: str,
                          adapter_dir_root: str, adapter_store: AdapterStore,
                          cancel_event: threading.Event) -> None:
    """Same contract as `trainer.run_training`, with the fine-tune in a child process."""
    spec_path = ""
    try:
        require_ml_installed()
        base = _trainer._resolve_base(cfg.base_model)   # needs the model registry, which only this process has
        handle, spec_path = tempfile.mkstemp(prefix="magnite-train-", suffix=".json")
        with os.fdopen(handle, "w", encoding="utf-8") as spec_file:
            json.dump({"cfg": cfg.model_dump(), "dataset_path": dataset_path,
                       "adapter_dir_root": adapter_dir_root, "job_id": job.id, "base": base}, spec_file)
        _supervise(job, cfg, spec_path, adapter_dir_root, adapter_store, cancel_event)
    except Exception as exc:  # noqa: BLE001
        mark_failed(job, str(exc))
    finally:
        if spec_path:
            try:
                os.unlink(spec_path)
            except OSError:
                pass
        if job.status != "done":   # nothing was registered, so no weights may be left behind
            shutil.rmtree(adapter_dir_for(adapter_dir_root, job.id), ignore_errors=True)


def _supervise(job, cfg, spec_path, adapter_dir_root, adapter_store, cancel) -> None:
    try:
        proc = subprocess.Popen(
            _default_train_command(spec_path), stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT, text=True, encoding="utf-8", errors="replace", bufsize=1)
    except OSError as exc:
        raise RuntimeError(f"Could not start the training process: {exc}") from exc

    with _live_lock:
        _live.add(proc)
    finished = threading.Event()
    watcher = threading.Thread(target=_watch_for_cancel, args=(proc, cancel, finished),
                               name="training-cancel-watch", daemon=True)
    watcher.start()
    result: dict | None = None
    error = ""
    noise: collections.deque[str] = collections.deque(maxlen=3)
    try:
        for line in proc.stdout:
            try:
                message = json.loads(line)
                if not isinstance(message, dict):
                    raise ValueError
            except ValueError:
                if line.strip():
                    noise.append(line.strip())
                continue
            if "error" in message:
                error = str(message["error"])
            elif "result" in message:
                result = message
            else:
                try:
                    apply_event(job, message)
                except Exception:  # noqa: BLE001 -- one malformed event must not end the job
                    logger.warning("Ignoring a malformed training event: %r", message)
        try:
            code = proc.wait(timeout=30)
        except subprocess.TimeoutExpired:
            proc.kill()
            code = proc.wait()
    finally:
        with _live_lock:
            _live.discard(proc)
        finished.set()
        watcher.join(timeout=5)
        for stream in (proc.stdin, proc.stdout):
            try:
                stream.close()
            except OSError:
                pass

    try:
        if result is not None and result.get("result") == "ok":
            publish_adapter(job, cfg, TrainOutcome(**result["outcome"]), adapter_store, cancel)
        elif cancel.is_set() or (result is not None and result.get("result") == "cancelled"):
            mark_cancelled(job)
        elif error:
            mark_failed(job, error)
        else:
            if noise:
                logger.warning("Training worker ended (code %s) after: %s", code, " | ".join(noise))
            detail = f" {noise[-1]}" if noise and code > 0 else ""
            mark_failed(job, describe_exit(code) + detail)
    except _Cancelled:
        mark_cancelled(job)
