"""Validate package provenance and write checksums for the exact tested bytes."""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import platform
import runpy
import subprocess
import time

REPO = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform", choices=("linux", "macos", "windows"), required=True)
    parser.add_argument("--architecture", choices=("x86_64", "arm64"), required=True)
    parser.add_argument("--source-sha", required=True)
    parser.add_argument("--build-info", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("packages", type=Path, nargs="+")
    args = parser.parse_args()
    identity = runpy.run_path(str(REPO / "backend/src/backend/_version.py"))
    info = json.loads((args.build_info / "build.json").read_text())
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=REPO, text=True).strip()
    expected = {"version": identity["RELEASE_VERSION"], "update_sequence": identity["UPDATE_SEQUENCE"],
                "data_schema": identity["DATA_SCHEMA"], "commit": args.source_sha,
                "working_tree_modified": False}
    if commit != args.source_sha or any(info.get(k) != v for k, v in expected.items()):
        parser.error("Package provenance does not match the clean, requested source identity")
    host_platform = {"Linux": "linux", "Darwin": "macos", "Windows": "windows"}[platform.system()]
    host_arch = {"AMD64": "x86_64", "aarch64": "arm64"}.get(platform.machine(), platform.machine())
    if (host_platform, host_arch) != (args.platform, args.architecture):
        parser.error("Package platform must match the native builder")
    args.output.mkdir(parents=True, exist_ok=True)
    assets = []
    for package in args.packages:
        if not package.is_file() or package.stat().st_size == 0:
            parser.error(f"Missing or empty tested package: {package}")
        with package.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        assets.append({"name": package.name, "size": package.stat().st_size, "sha256": digest})
    if len({a["name"] for a in assets}) != len(assets):
        parser.error("Duplicate package names")
    label = f"{args.platform}-{args.architecture}"
    evidence = {**info, "package_platform": args.platform, "package_architecture": args.architecture,
                "assets": assets, "evidence_created_at": int(time.time())}
    (args.output / f"provenance-{label}.json").write_text(json.dumps(evidence, indent=2) + "\n")
    (args.output / f"SHA256SUMS-{label}").write_text("".join(f"{a['sha256']}  {a['name']}\n" for a in assets))


if __name__ == "__main__":
    main()
