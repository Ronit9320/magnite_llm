"""Stage a relocatable Python runtime, never the developer's virtualenv.

Requires uv and an Astral python-build-standalone installation. Use --python-root
to reuse an existing standalone installation, otherwise uv downloads Python.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

from record_build import record_build

REPO = Path(__file__).resolve().parents[1]


def run(*args, **kwargs):
    print("+", " ".join(map(str, args)), flush=True)
    return subprocess.run(list(map(str, args)), check=True, **kwargs)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--python-root", type=Path)
    parser.add_argument("--resume", action="store_true", help="Retry an interrupted dependency install")
    args = parser.parse_args()
    dest = args.destination.resolve()
    dest.mkdir(parents=True, exist_ok=True)
    runtime = dest / "runtime" / "python"
    if runtime.exists() and not args.resume:
        parser.error(f"Runtime already exists: {runtime}; choose a fresh destination")
    uv = shutil.which("uv")
    if not uv:
        parser.error("Install uv before building the runtime")
    source = args.python_root
    if source is None and not runtime.exists():
        downloads = REPO / "build" / "packaging" / "python-download"
        run(uv, "python", "install", "3.12.12", "--install-dir", downloads, "--no-bin")
        candidates = list(downloads.glob("cpython-3.12.12-*"))
        if len(candidates) != 1:
            parser.error(f"Expected one Python installation under {downloads}")
        source = candidates[0]
    if not runtime.exists():
        shutil.copytree(source, runtime, symlinks=True,
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc"))
    python = runtime / ("python.exe" if sys.platform == "win32" else "bin/python3")
    env = os.environ.copy()
    env.pop("PYTHONPATH", None)
    env.pop("PYTHONHOME", None)
    env["UV_CACHE_DIR"] = str(REPO / "build" / "packaging" / "uv-cache")
    # Never compile llama.cpp for the build host's particular CPU/GPU.
    env["CMAKE_ARGS"] = (
        "-DGGML_NATIVE=OFF -DGGML_CUDA=OFF -DGGML_AVX2=OFF "
        "-DGGML_AVX512=OFF -DGGML_FMA=OFF -DGGML_F16C=OFF "
        "-DLLAMA_CURL=OFF"
    )
    env["CMAKE_BUILD_PARALLEL_LEVEL"] = "2"
    run(uv, "pip", "install", "--python", python, "--break-system-packages",
        "--torch-backend", "cpu", "--no-binary", "llama-cpp-python",
        "--reinstall-package", "magnite-backend",
        "--constraint", REPO / "packaging/constraints-cpu.txt",
        "-r", REPO / "packaging/requirements-cpu.txt", REPO / "backend", env=env)
    run(python, "-I", "-B", "-c",
        "import backend.server, llama_cpp, torch, transformers, peft, "
        "sentence_transformers, datasets; "
        "assert not torch.version.cuda; print('CPU runtime imports OK')", env=env)
    record_build(dest)
    notices = dest / "licenses"
    notices.mkdir(exist_ok=True)
    for name in ("LICENSE", "THIRD_PARTY_NOTICES.md"):
        shutil.copy2(REPO / name, notices / name)
    for path in (REPO / "app/resources/fonts").glob("OFL-*.txt"):
        shutil.copy2(path, notices / path.name)
    shutil.copy2(REPO / "packaging/TESTING.md", dest / "TESTING.md")
    print(f"Runtime staged at {runtime}")


if __name__ == "__main__":
    main()
