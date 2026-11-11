"""Optional runtime packages: a fixed catalogue, honest status, and a user-writable install location.

Nothing here accepts a package name from a caller. Installs go to ``<app_home>/packages`` so the
signed application bundle is never modified, and that directory is *appended* to ``sys.path`` so a
bundled copy always wins over an installed one.
"""
from __future__ import annotations

import importlib.metadata
import importlib.util
import os
import platform
import shutil
import signal
import subprocess
import sys
import threading
from dataclasses import dataclass
from pathlib import Path

from backend.paths import app_home


@dataclass(frozen=True)
class Package:
    id: str
    label: str
    role: str
    distribution: str
    spec: str
    size_hint: str
    requires: tuple[str, ...] = ()


@dataclass(frozen=True)
class Installer:
    kind: str          # "pip" or "uv"
    executable: str = ""


CATALOGUE: tuple[Package, ...] = (
    Package("llama.cpp", "llama.cpp", "Chat engine for GGUF models", "llama-cpp-python",
            "llama-cpp-python>=0.3.0", "May compile on install; needs a C/C++ toolchain"),
    Package("vLLM", "vLLM", "High-throughput GPU model server", "vllm",
            "vllm", "Several GB; needs an NVIDIA GPU"),
    Package("torch", "PyTorch", "Training, adapters and Transformers inference", "torch",
            "torch>=2.1.0", "Large download, from 200 MB to several GB"),
    Package("PEFT", "PEFT", "LoRA adapters for Transformers models", "peft",
            "peft>=0.7.0", "Small, but pulls in PyTorch dependencies", requires=("torch",)),
)
_BY_ID = {package.id: package for package in CATALOGUE}


def get(package_id: str) -> Package | None:
    return _BY_ID.get(package_id)


def packages_dir() -> Path:
    return Path(app_home()) / "packages"


def activate() -> None:
    """Make installed packages importable. Appended, so bundled copies are never shadowed."""
    target = str(packages_dir())
    if target not in sys.path:
        sys.path.append(target)
    importlib.invalidate_caches()


def installed_version(distribution: str) -> str | None:
    try:
        return importlib.metadata.version(distribution)
    except importlib.metadata.PackageNotFoundError:
        return None


def find_installer() -> Installer | None:
    """pip when this interpreter has it (packaged runtime); otherwise uv for source checkouts."""
    if importlib.util.find_spec("pip") is not None:
        return Installer("pip", sys.executable)
    uv = shutil.which("uv")
    return Installer("uv", uv) if uv else None


def platform_support(package: Package) -> tuple[bool, str | None]:
    if package.id == "vLLM":
        system, machine = platform.system(), platform.machine().lower()
        if system != "Linux" or machine not in ("x86_64", "amd64"):
            name = {"Darwin": "macOS"}.get(system, system or "this platform")
            return False, f"Not available on {name}"
    return True, None


def status() -> list[dict]:
    installer = find_installer()
    versions = {package.id: installed_version(package.distribution) for package in CATALOGUE}
    rows = []
    for package in CATALOGUE:
        version = versions[package.id]
        state, reason = "installed", None
        if version is None:
            supported, reason = platform_support(package)
            missing = [_BY_ID[r].label for r in package.requires if versions.get(r) is None]
            if not supported:
                state = "unsupported"
            elif missing:
                state, reason = "blocked", f"Needs {', '.join(missing)} first"
            elif installer is None:
                state, reason = "missing", "No installer is available in this runtime"
            else:
                state = "missing"
        rows.append({"id": package.id, "label": package.label, "role": package.role,
                     "installed_version": version, "state": state, "reason": reason,
                     "size_hint": package.size_hint,
                     "installable": state == "missing" and installer is not None})
    return rows


class InstallRefused(Exception):
    def __init__(self, status_code: int, detail: str):
        super().__init__(detail)
        self.status_code, self.detail = status_code, detail


def build_command(installer: Installer, package: Package, target: Path) -> list[str]:
    """The installer command line comes only from the catalogue, never from request data."""
    if installer.kind == "uv":
        return [installer.executable, "pip", "install", "--python", sys.executable,
                "--target", str(target), package.spec]
    return [installer.executable, "-B", "-I", "-m", "pip", "install", "--disable-pip-version-check",
            "--no-input", "--progress-bar", "off", "--target", str(target), package.spec]


def busy_reason() -> str:
    """Why an install must wait ('' when the backend is idle). Unreadable state counts as busy."""
    try:
        from backend.inference.supervisor import get_supervisor
        from backend.routes.models import _get_registry
        from backend.routes.training import _get_manager
        if any(job.status in ("queued", "running") for job in _get_manager().list()):
            return "Training is queued or running. Finish it before installing packages."
        if _get_registry().has_active_pulls():
            return "A model download is in progress. Finish it before installing packages."
        if get_supervisor().status().loaded_model:
            return "Unload the current model before installing packages."
    except Exception:  # noqa: BLE001 - refusing is safer than installing into an unknown state
        return "Could not confirm that the backend is idle."
    return ""


def _refresh_engines() -> None:
    try:
        from backend.inference.supervisor import get_supervisor
        get_supervisor().refresh_availability()
    except Exception:  # noqa: BLE001 - a stale flag only means the app needs a restart
        pass


class InstallManager:
    """One cancellable install at a time; the log is bounded and redacted before it leaves."""
    _RAW_LOG = 8192
    _TAIL = 4096

    def __init__(self, popen=subprocess.Popen):
        self._popen = popen
        self._lock = threading.Lock()
        self._thread: threading.Thread | None = None
        self._process = None
        self._id: str | None = None
        self._state = "idle"
        self._log = ""
        self._cancelled = False

    def status(self) -> dict:
        from backend.routes.system import redact_log
        with self._lock:
            return {"id": self._id, "state": self._state,
                    "log_tail": redact_log(self._log)[-self._TAIL:]}

    def wait(self, timeout: float | None = None) -> None:
        thread = self._thread
        if thread is not None:
            thread.join(timeout)

    def start(self, package_id: str) -> None:
        package = get(package_id)
        if package is None:
            raise InstallRefused(404, "Unknown package")
        row = next(row for row in status() if row["id"] == package_id)
        if row["state"] == "installed":
            raise InstallRefused(409, f"{package.label} is already installed")
        if not row["installable"]:
            raise InstallRefused(409, row["reason"] or f"{package.label} cannot be installed here")
        installer = find_installer()
        with self._lock:
            if self._state == "running":
                raise InstallRefused(409, "An install is already running")
            busy = busy_reason()
            if busy:
                raise InstallRefused(409, busy)
            target = packages_dir()
            target.mkdir(parents=True, exist_ok=True)
            # The installer runs build scripts and downloads; it never inherits Magnite's own settings.
            env = {key: value for key, value in os.environ.items() if not key.startswith("MAGNITE_")}
            kwargs = {"stdout": subprocess.PIPE, "stderr": subprocess.STDOUT, "text": True,
                      "env": env, "start_new_session": os.name == "posix"}
            self._process = self._popen(build_command(installer, package, target), **kwargs)
            self._id, self._state, self._log, self._cancelled = package_id, "running", "", False
            self._thread = threading.Thread(target=self._run, args=(self._process, package),
                                            name="package-install", daemon=True)
            self._thread.start()

    def cancel(self) -> bool:
        with self._lock:
            if self._state != "running" or self._process is None:
                return False
            self._cancelled = True
            process = self._process
        try:
            if self._popen is subprocess.Popen and os.name == "posix":
                os.killpg(os.getpgid(process.pid), signal.SIGTERM)   # pip build children too
            else:
                process.terminate()
        except (OSError, ProcessLookupError):
            pass
        return True

    def _run(self, process, package: Package) -> None:
        try:
            for line in process.stdout or ():
                with self._lock:
                    self._log = (self._log + line)[-self._RAW_LOG:]
            code = process.wait()
        except Exception as exc:  # noqa: BLE001
            with self._lock:
                self._log += f"\nInstaller error: {type(exc).__name__}"
            code = -1
        state = "failed"
        with self._lock:
            cancelled = self._cancelled
        if cancelled:
            state = "cancelled"
        elif code == 0:
            activate()
            if installed_version(package.distribution) is not None:
                _refresh_engines()
                state = "done"
            else:
                with self._lock:
                    self._log += f"\n{package.label} was installed but could not be found afterwards."
        with self._lock:
            self._state = state
            self._process = None


_manager: InstallManager | None = None


def get_manager() -> InstallManager:
    global _manager
    if _manager is None:
        _manager = InstallManager()
    return _manager
