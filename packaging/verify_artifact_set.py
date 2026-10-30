"""Require one tested native package per platform, all from one clean source SHA."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import sys

from identity import identity

REQUIRED = {("linux", "x86_64"), ("macos", "arm64"), ("windows", "x86_64")}


def verify(root: Path, source_sha: str):
    canonical = identity()
    seen = set()
    for path in root.rglob("provenance-*.json"):
        info = json.loads(path.read_text())
        target = (info.get("package_platform"), info.get("package_architecture"))
        if target not in REQUIRED or target in seen:
            raise ValueError(f"Unexpected or duplicate platform: {target}")
        seen.add(target)
        expected = {"version": canonical["version"], "update_sequence": canonical["sequence"],
                    "data_schema": canonical["data_schema"], "commit": source_sha,
                    "working_tree_modified": False}
        if any(info.get(k) != v for k, v in expected.items()):
            raise ValueError(f"Source identity mismatch: {path}")
        if len(info.get("assets", [])) != 1:
            raise ValueError(f"Expected exactly one tested package for {target}")
        for asset in info["assets"]:
            name = asset["name"]
            if Path(name).name != name or "/" in name or "\\" in name:
                raise ValueError("Unsafe package filename")
            package = path.parent / name
            if package.is_symlink() or not package.is_file() or package.stat().st_size != asset["size"]:
                raise ValueError(f"Missing or changed tested package: {package}")
            with package.open("rb") as stream:
                digest = hashlib.file_digest(stream, "sha256").hexdigest()
            if digest != asset["sha256"]:
                raise ValueError(f"Package digest mismatch: {package}")
    if seen != REQUIRED:
        raise ValueError(f"Missing platform artifacts: {REQUIRED - seen}")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("root", type=Path)
    args = parser.parse_args()
    try:
        verify(args.root, args.source_sha)
    except (ValueError, OSError, KeyError) as exc:
        sys.exit(str(exc))
    print(f"PASS: all three native packages match clean source {args.source_sha}")
