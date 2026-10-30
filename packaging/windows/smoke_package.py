"""Install, relocate, launch, upgrade and uninstall a disposable Windows package."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

import psutil


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--installer", type=Path, required=True)
    parser.add_argument("--logs", type=Path, required=True)
    args = parser.parse_args()
    installer = args.installer.resolve()
    logs = args.logs.resolve()
    logs.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="Magnite installation test ") as temporary:
        root = Path(temporary)
        installed = root / "Installed App With Spaces"
        env = {**os.environ, "MAGNITE_HOME": str(root / "data"), "HF_HOME": str(root / "hf"),
               "QT_QPA_PLATFORM": "offscreen", "PYTHONHOME": str(root / "broken-python"),
               "PYTHONPATH": str(root / "broken-modules"), "QT_FORCE_STDERR_LOGGING": "1",
               "PATH": os.path.join(os.environ["SystemRoot"], "System32")}

        def run(name, command, *, marker=None, timeout=240):
            with (logs / f"{name}.log").open("w") as output:
                result = subprocess.run([str(a) for a in command], env=env, cwd=root,
                                        stdout=output, stderr=subprocess.STDOUT, timeout=timeout)
            content = (logs / f"{name}.log").read_text(errors="replace")
            if result.returncode != 0 or (marker and marker not in content):
                raise RuntimeError(f"{name} failed ({result.returncode}); see {logs / (name + '.log')}")
            print(f"PASS {name}", flush=True)

        install_args = [installer, "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/RESTARTEXITCODE=8",
                        f"/DIR={installed}", f"/LOG={logs / 'installer-details.log'}"]
        run("install", install_args)
        exe = installed / "magnite-app.exe"
        python = installed / "runtime/python/python.exe"
        if not python.is_file():
            raise RuntimeError("Installer omitted the isolated Python runtime")
        run("theme", [exe, "--theme-check"], marker="0 failure(s)")
        run("ui", [exe, "--ui-check"], marker="PASS (0 failures)")
        run("desktop", [exe, "--selftest"], marker="PASS (0 failures)")
        sentinel = root / "data/preserved.txt"
        sentinel.write_text("persistent data outside the installation")
        run("upgrade", install_args)
        if sentinel.read_text() != "persistent data outside the installation":
            raise RuntimeError("Upgrade changed application data")
        run("desktop-after-upgrade", [exe, "--selftest"], marker="PASS (0 failures)")
        # Kill a running desktop and verify the Windows job contains its backend
        # descendants observed during startup, without relying on process names.
        with (logs / "crash-lifecycle.log").open("w") as output:
            gui = subprocess.Popen([str(exe)], env=env, cwd=root, stdout=output, stderr=subprocess.STDOUT)
            children = []
            try:
                deadline = time.monotonic() + 90
                while time.monotonic() < deadline and gui.poll() is None:
                    children = psutil.Process(gui.pid).children(recursive=True)
                    if children and "backend health OK" in (logs / "crash-lifecycle.log").read_text(errors="replace"):
                        break
                    time.sleep(0.2)
                else:
                    raise RuntimeError("Desktop backend did not become ready before crash check")
                gui.kill()
                gui.wait(10)
                _, alive = psutil.wait_procs(children, timeout=15)
                if alive:
                    raise RuntimeError(f"Backend processes survived desktop crash: {[p.pid for p in alive]}")
            finally:
                if gui.poll() is None:
                    gui.kill()
                    gui.wait(10)
                for child in children:
                    if child.is_running():
                        child.kill()
        print("PASS desktop-crash-cleans-process-tree", flush=True)
        run("restart-after-crash", [exe, "--selftest"], marker="PASS (0 failures)")
        run("uninstall", [installed / "unins000.exe", "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART",
                          f"/LOG={logs / 'uninstaller-details.log'}"])
        if exe.exists() or not sentinel.is_file():
            raise RuntimeError("Uninstall did not remove the app or changed external data")
        (logs / "result.json").write_text(json.dumps({"result": "pass", "installation_path_with_spaces": True,
            "upgrade_preserves_data": True, "uninstall_preserves_data": True,
            "crash_process_tree_cleanup": True, "host_python_poisoned": True}, indent=2) + "\n")


if __name__ == "__main__":
    main()
