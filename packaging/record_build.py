"""Record the exact source and runtime for every packaging invocation."""
from __future__ import annotations

import json
import os
from pathlib import Path
import platform
import subprocess
import sys
import runpy


def record_build(destination: Path):
    repo = Path(__file__).resolve().parents[1]
    python = destination / "runtime/python" / ("python.exe" if sys.platform == "win32" else "bin/python3")
    info = destination / "build-info"
    info.mkdir(exist_ok=True)
    packages = subprocess.check_output([str(python), "-I", "-B", "-c",
        "import importlib.metadata as m; "
        "print('\\n'.join(sorted(d.metadata['Name']+'=='+d.version for d in m.distributions())))"], text=True)
    (info / "python-packages.txt").write_text(packages, encoding="utf-8")
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=repo, text=True).strip()
    dirty = bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=repo))
    if os.environ.get("MAGNITE_REQUIRE_CLEAN") == "1" and dirty:
        raise RuntimeError("Release packages require a clean source checkout")
    identity = runpy.run_path(str(repo / "backend/src/backend/_version.py"))
    (info / "build.json").write_text(json.dumps({
        "version": identity["RELEASE_VERSION"],
        "update_sequence": identity["UPDATE_SEQUENCE"],
        "data_schema": identity["DATA_SCHEMA"],
        "commit": commit, "working_tree_modified": dirty,
        "platform": platform.platform(), "architecture": platform.machine(),
        "host_libc": platform.libc_ver(),
        "runtime": ("Python 3.12.12; macOS arm64 torch wheel (CPU/MPS); llama-cpp-python built from "
                    "sdist with GGML_METAL=ON (embedded shaders); ML extras without bitsandbytes"
                    if sys.platform == "darwin" else
                    "Python 3.12.12; CPU torch and llama.cpp; ML extras"),
    }, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    record_build(Path(sys.argv[1]).resolve())
