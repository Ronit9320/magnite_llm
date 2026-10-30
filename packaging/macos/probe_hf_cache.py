"""ISSUE-007 probe: how the backend's HF-cache scan and delete behave on a shared-blobs cache.

huggingface_hub >= 1.33 can store blobs once at <hub>/blobs/xx/<sha> and make each repo's
blobs/<sha> a symlink to it. The bundled runtime pins 1.30.0. Run this with each interpreter on
its own APFS clone of a real cache (never the original):

    cp -c -R ~/.cache/huggingface/hub "$W/hub-130"
    "$R" -I -B packaging/macos/probe_hf_cache.py "$W/hub-130" --delete HuggingFaceTB/SmolLM2-135M-Instruct
    PYTHONPATH=backend/src .venv/bin/python packaging/macos/probe_hf_cache.py "$W/hub-133" --delete ...

Prints one JSON document: hub version, scan warnings, repos with size/files, and after the delete
(through ModelRegistry._remove_hf, the app's code), whether every other repo still resolves and how
many bytes of the shared store were left unreferenced.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
from types import SimpleNamespace


def scan(hub: Path) -> dict:
    import huggingface_hub
    from huggingface_hub import scan_cache_dir
    cache = scan_cache_dir(hub)
    return {
        "huggingface_hub": huggingface_hub.__version__,
        "warnings": [str(w) for w in cache.warnings],
        "repos": {r.repo_id: {"size_on_disk": r.size_on_disk, "nb_files": r.nb_files,
                              "revisions": len(r.revisions)} for r in cache.repos},
    }


def integrity(hub: Path) -> dict:
    dangling = [str(p.relative_to(hub)) for p in hub.rglob("*") if p.is_symlink() and not p.exists()]
    shared = hub / "blobs"
    referenced = set()
    for repo in hub.glob("models--*"):
        for link in (repo / "blobs").glob("*"):
            if link.is_symlink():
                referenced.add(link.resolve())
    unreferenced = [p for p in shared.rglob("*") if p.is_file() and not p.name.startswith(".")
                    and p.resolve() not in referenced] if shared.is_dir() else []
    return {"dangling_symlinks": dangling,
            "shared_blobs_unreferenced": len(unreferenced),
            "shared_bytes_unreferenced": sum(p.stat().st_size for p in unreferenced)}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("hub", type=Path)
    ap.add_argument("--delete", help="repo id whose (single) snapshot is removed via ModelRegistry._remove_hf")
    args = ap.parse_args()
    hub = args.hub.resolve()
    if hub == (Path.home() / ".cache/huggingface/hub").resolve():
        raise SystemExit("refusing to run on the real cache; pass a clone")
    report = {"before": scan(hub), "integrity_before": integrity(hub)}
    if args.delete:
        from backend.models.registry import ModelRegistry
        snapshots = sorted((hub / ("models--" + args.delete.replace("/", "--")) / "snapshots").iterdir())
        try:
            ModelRegistry._remove_hf(None, SimpleNamespace(path=str(snapshots[0])))
            report["delete"] = {"repo": args.delete, "result": "ok"}
        except Exception as exc:  # noqa: BLE001
            report["delete"] = {"repo": args.delete, "result": f"{type(exc).__name__}: {exc}"}
        report["after"] = scan(hub)
        report["integrity_after"] = integrity(hub)
        repo_dir = hub / ("models--" + args.delete.replace("/", "--"))
        report["deleted_repo_dir_left"] = sorted(str(p.relative_to(hub)) for p in repo_dir.rglob("*")) if repo_dir.exists() else []
    print(json.dumps(report, indent=1))


if __name__ == "__main__":
    main()
