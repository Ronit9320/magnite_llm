"""Exercise a relocated AppImage without the development Python environment."""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("appimage", type=Path)
    parser.add_argument("--logs", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--features", action="store_true", help="Download tiny models and test inference/LoRA/RAG")
    args = parser.parse_args()
    logs = args.logs.resolve()
    logs.mkdir(parents=True, exist_ok=True)
    cache = args.cache.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    repo = Path(__file__).resolve().parents[1]

    with tempfile.TemporaryDirectory(prefix="magnite-relocated-") as temporary:
        root = Path(temporary)
        image = root / "Magnite.AppImage"
        shutil.copy2(args.appimage.resolve(), image)
        image.chmod(0o755)
        env = os.environ.copy()
        env.update({
            "QT_QPA_PLATFORM": "offscreen",
            "MAGNITE_HOME": str(root / "data"),
            "XDG_CONFIG_HOME": str(root / "config"),
            "HF_HOME": str(cache),
            # Prove that the packaged client ignores a broken host Python setup.
            "PYTHONHOME": str(root / "nonexistent-python"),
            "PYTHONPATH": str(root / "nonexistent-modules"),
            "PATH": "/usr/bin:/bin",
        })
        env.pop("LD_LIBRARY_PATH", None)
        env.pop("QT_PLUGIN_PATH", None)

        def check(name, command, timeout=180, environment=None):
            print(f"Running {name}", flush=True)
            with (logs / f"{name}.log").open("w") as output:
                subprocess.run(list(map(str, command)), cwd=root,
                               env=environment or env, stdout=output,
                               stderr=subprocess.STDOUT, check=True, timeout=timeout)
            print(f"PASS {name}", flush=True)

        check("extract", [image, "--appimage-extract"])
        appdir = root / "squashfs-root"
        launcher = appdir / "AppRun"
        check("theme", [launcher, "--theme-check"])
        check("ui", [launcher, "--ui-check"])
        check("desktop", [launcher, "--selftest"])
        if args.features:
            feature_env = {**env, "MAGNITE_SELFTEST_INFERENCE": "1"}
            check("gguf-inference", [launcher, "--selftest"], 600, feature_env)
            python = appdir / "usr/bin/runtime/python/bin/python3"
            feature_env = {**env, "LD_LIBRARY_PATH": str(appdir / "usr/lib")}
            check("lora-rag-workflow", [python, "-I", "-B", repo / "backend/tests/live_workflow.py"],
                  600, feature_env)
        # Exercise the actual AppImage launcher too, without relying on FUSE.
        check("appimage-launcher", [image, "--appimage-extract-and-run", "--theme-check"])


if __name__ == "__main__":
    main()
