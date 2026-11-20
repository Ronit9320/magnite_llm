"""Install, relocate, launch, upgrade and uninstall a disposable Windows package."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import runpy
import subprocess
import tempfile
import time

import psutil


def features():
    """The build profile; runpy keeps this working under an isolated (-I) interpreter."""
    return runpy.run_path(str(Path(__file__).resolve().parents[1] / "identity.py"))["features"]()


def in_app_update(installed: Path, installer: Path, root: Path, env: dict, logs: Path):
    """Run the bundled update script the way the client starts it after it has quit."""
    version = runpy.run_path(str(Path(__file__).resolve().parents[1] / "identity.py"))["identity"]()["version"]
    transaction = Path(tempfile.mkdtemp(prefix=".magnite-update-", dir=root))
    # A finished process stands in for the client, which quits before the script installs.
    parent = subprocess.Popen([os.path.join(os.environ["SystemRoot"], "System32", "cmd.exe"), "/c", "exit"], env=env)
    parent.wait(30)
    powershell = os.path.join(os.environ["SystemRoot"], "System32", "WindowsPowerShell", "v1.0", "powershell.exe")
    script = Path(__file__).resolve().with_name("update_install.ps1")
    command = [powershell, "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", script,
               "-Installer", installer, "-AppDir", installed, "-Transaction", transaction,
               "-ParentPid", str(parent.pid), "-Version", version]
    try:
        with (logs / "in-app-update.log").open("w") as output:
            result = subprocess.run([str(part) for part in command], env=env, cwd=root, stdout=output,
                                    stderr=subprocess.STDOUT, timeout=420)
        status = (transaction / "status").read_text(errors="replace") if (transaction / "status").exists() else ""
        for name in ("installer.log", "setup.log", "status"):
            if (transaction / name).exists():
                (logs / f"in-app-update-{name}").write_bytes((transaction / name).read_bytes())
        if result.returncode != 0 or status != f"success: {version}":
            raise RuntimeError(f"In-app update script failed ({result.returncode}): {status!r}; see {logs}")
    finally:
        # The script leaves the updated app running, as it would for a user.
        for process in psutil.process_iter(["exe"]):
            try:
                if process.info["exe"] and Path(process.info["exe"]).resolve().is_relative_to(installed.resolve()):
                    process.kill()
            except (psutil.Error, OSError):
                pass
    print("PASS in-app-update-script", flush=True)


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

        def run(name, command, *, marker=None, timeout=240, extra_env=None):
            with (logs / f"{name}.log").open("w") as output:
                result = subprocess.run([str(a) for a in command], env={**env, **(extra_env or {})}, cwd=root,
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
        smoke_pi = Path(__file__).resolve().parents[1] / "smoke_pi.py"
        run("bundled-pi", [python, "-I", "-B", smoke_pi], marker="Bundled Node and Pi bridge OK")
        run("theme", [exe, "--theme-check"], marker="0 failure(s)")
        run("ui", [exe, "--ui-check"], marker="PASS (0 failures)", timeout=900)
        run("desktop", [exe, "--selftest"], marker="PASS (0 failures)")
        if features()["blueprints_and_tools"]:
            run("blueprints", [exe, "--ui-check"], marker="[blueprints-live] PASS (0 failures)",
                extra_env={"MAGNITE_BLUEPRINTS_LIVE_CHECK": "1"})
        sentinel = root / "data/preserved.txt"
        sentinel.write_text("persistent data outside the installation")
        run("upgrade", install_args)
        if sentinel.read_text() != "persistent data outside the installation":
            raise RuntimeError("Upgrade changed application data")
        run("desktop-after-upgrade", [exe, "--selftest"], marker="PASS (0 failures)")
        in_app_update(installed, installer, root, env, logs)
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
                backends = [child for child in children if "backend.main" in child.cmdline()]
                if len(backends) != 1:
                    raise RuntimeError("Expected exactly one installed backend process")
                for child in backends:
                    if not Path(child.exe()).resolve().is_relative_to(installed.resolve()):
                        raise RuntimeError(f"Host executable leaked into installed process tree: {child.exe()}")
                qt_modules = [entry.path for entry in psutil.Process(gui.pid).memory_maps()
                              if Path(entry.path).name.lower().startswith("qt6") and entry.path.lower().endswith(".dll")]
                if not qt_modules or any(not Path(path).resolve().is_relative_to(installed.resolve()) for path in qt_modules):
                    raise RuntimeError(f"Qt DLLs did not load from the installed package: {qt_modules}")
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
            "crash_process_tree_cleanup": True, "host_python_poisoned": True,
            "in_app_update_script": True}, indent=2) + "\n")


if __name__ == "__main__":
    main()
