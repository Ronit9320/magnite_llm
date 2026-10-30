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
    parser.add_argument("--requirements", type=Path, default=REPO / "packaging/requirements-cpu.txt",
                        help="Pinned requirements file (default: the Linux CPU set)")
    parser.add_argument("--constraints", type=Path,
                        help="Full lock for `uv pip install -c` (default: constraints-*.txt beside --requirements; "
                             "regenerate with packaging/lock_requirements.sh)")
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
    env.setdefault("UV_CACHE_DIR", str(REPO / "build" / "packaging" / "uv-cache"))
    # Never compile llama.cpp for the build host's particular CPU/GPU.
    macos = sys.platform == "darwin"
    if macos:
        # Apple silicon: Metal with the shader library embedded (no Metal compiler is
        # needed to build; shaders compile at load). Pin the deployment target so the
        # compiled libraries never require a newer macOS than the app declares.
        # LLAMA_OPENSSL=OFF: otherwise CMake links whatever OpenSSL the build host has
        # (Homebrew's), which a user's Mac will not. Python downloads use huggingface_hub.
        env["CMAKE_ARGS"] = (
            "-DGGML_METAL=ON -DGGML_METAL_EMBED_LIBRARY=ON -DGGML_NATIVE=OFF "
            "-DGGML_CUDA=OFF -DLLAMA_CURL=OFF -DLLAMA_OPENSSL=OFF "
            "-DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0"
        )
        env["MACOSX_DEPLOYMENT_TARGET"] = "14.0"
        # Keep the build host's paths out of __FILE__ strings in the shipped libraries.
        for flags in ("CFLAGS", "CXXFLAGS", "OBJCFLAGS"):
            env[flags] = f"-ffile-prefix-map={REPO}=. {env.get(flags, '')}".strip()
        env["CMAKE_BUILD_PARALLEL_LEVEL"] = "4"
    else:
        env["CMAKE_ARGS"] = (
            "-DGGML_NATIVE=OFF -DGGML_CUDA=OFF -DGGML_AVX2=OFF "
            "-DGGML_AVX512=OFF -DGGML_FMA=OFF -DGGML_F16C=OFF "
            "-DGGML_METAL=OFF -DLLAMA_CURL=OFF -DLLAMA_OPENSSL=OFF"
        )
        env["CMAKE_BUILD_PARALLEL_LEVEL"] = "2"
    # macOS has no separate CPU torch build: the standard wheel provides CPU and MPS.
    torch_backend = [] if macos else ["--torch-backend", "cpu"]
    constraints = args.constraints or args.requirements.with_name(
        args.requirements.name.replace("requirements-", "constraints-", 1))
    if not constraints.is_file():
        parser.error(f"Required dependency lock is missing: {constraints}")
    lock = ["-c", constraints]
    run(uv, "pip", "install", "--python", python, "--break-system-packages",
        *torch_backend, "--no-binary", "llama-cpp-python",
        "--reinstall-package", "magnite-backend",
        "-r", args.requirements, *lock, REPO / "backend", env=env)
    run(python, "-I", "-B", "-c",
        "import backend.server, llama_cpp, torch, transformers, peft, "
        "sentence_transformers, datasets; "
        "assert not torch.version.cuda; print('" + ("runtime" if macos else "CPU runtime") + " imports OK')", env=env)
    record_build(dest)
    notices = dest / "licenses"
    notices.mkdir(exist_ok=True)
    for name in ("LICENSE", "THIRD_PARTY_NOTICES.md"):
        shutil.copy2(REPO / name, notices / name)
    for path in (REPO / "app/resources/fonts").glob("OFL-*.txt"):
        shutil.copy2(path, notices / path.name)
    testing = REPO / ("packaging/macos/TESTING.md" if macos else
                      "packaging/windows/TESTING.md" if sys.platform == "win32" else "packaging/TESTING.md")
    if testing.exists():
        shutil.copy2(testing, dest / "TESTING.md")
    else:
        print(f"note: {testing.relative_to(REPO)} not written yet; not bundled")
    print(f"Runtime staged at {runtime}")


if __name__ == "__main__":
    main()
