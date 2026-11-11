"""Child process that runs one LoRA/QLoRA fine-tune and reports progress.

The backend runs this separately so that a training crash (out of memory, a native
fault) ends only this process, not chat, RAG and the model library with it. It follows
the downloader's pattern (`backend.models.pull_worker`): the parent applies the events,
registers the adapter itself and kills this process if a cancel does not take effect.

Protocol, one JSON object per stdout line (anything else, blank lines included, is ignored
by the parent; every line is preceded by a newline because stderr shares the pipe):

    {"status"|"message"|"progress"|"current_step"|"total_steps": ...}   any subset
    {"log": {"step", "logs", "elapsed", "epoch"}}                       one per logged step
    {"result": "ok", "outcome": {...TrainOutcome...}}                   weights saved
    {"result": "cancelled"}                                             stopped on request
    {"error": "..."}                                                    failed

A line "cancel" on stdin sets the cancel event. Exit codes: 0 ok or cancelled, 1 error,
2 bad usage.

Usage: python -m backend.training.train_worker <spec.json>
       spec = {"cfg": TrainingConfig, "dataset_path", "adapter_dir_root", "job_id", "base"}
       "base" is the base model the backend already resolved (this process has no model registry).
"""
from __future__ import annotations

import dataclasses
import json
import sys
import threading

from backend.models.pull_worker import _start_parent_watch
from backend.training.schemas import TrainingConfig
from backend.training.trainer import _Cancelled, train_lora


class _Writer:
    """Writes protocol lines; a vanished parent is not an error here (the parent watch ends us)."""

    def __init__(self, out):
        self._out = out
        self._lock = threading.Lock()

    def __call__(self, payload: dict) -> None:
        with self._lock:
            try:
                # Leading newline: stderr shares the pipe, and a library's unterminated fragment
                # (a progress bar, a warning) must not swallow this line.
                self._out.write("\n" + json.dumps(payload) + "\n")
                self._out.flush()
            except (OSError, ValueError):
                pass


def _watch_for_cancel(stdin, cancel: threading.Event) -> None:
    def read():
        try:
            for line in stdin:
                if line.strip() == "cancel":
                    cancel.set()
        except (OSError, ValueError):
            pass
    threading.Thread(target=read, name="cancel-reader", daemon=True).start()


def main(argv: list[str] | None = None, out=None, stdin=None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    write = _Writer(out if out is not None else sys.stdout)
    if len(argv) != 1:
        write({"error": "usage: train_worker <spec.json>"})
        return 2
    try:
        with open(argv[0], "r", encoding="utf-8") as handle:
            spec = json.load(handle)
        cfg = TrainingConfig(**spec["cfg"])
        dataset_path, adapter_dir_root, job_id = spec["dataset_path"], spec["adapter_dir_root"], spec["job_id"]
        base = spec.get("base")
    except (OSError, ValueError, KeyError, TypeError) as exc:
        write({"error": f"bad training spec: {exc}"})
        return 2

    _start_parent_watch()
    cancel = threading.Event()
    _watch_for_cancel(stdin if stdin is not None else sys.stdin, cancel)
    try:
        outcome = train_lora(cfg, dataset_path, adapter_dir_root, job_id, write, cancel, base=base)
    except _Cancelled:
        write({"result": "cancelled"})
        return 0
    except Exception as exc:  # noqa: BLE001
        write({"error": str(exc) or type(exc).__name__})
        return 1
    write({"result": "ok", "outcome": dataclasses.asdict(outcome)})
    return 0


if __name__ == "__main__":
    sys.exit(main())
