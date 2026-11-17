"""Memory guard for embedding work.

Embedding models run on the same machine as the chat model, and on Apple Silicon GPU memory is
shared RAM that ordinary "resident size" numbers do not show. An unguarded run once pushed an
8 GB Mac into swap and a watchdog panic, so every embedding loop checks these limits and stops
with a clear error instead of dragging the system down.

No ML imports here; torch is touched only if something else already imported it.
"""
from __future__ import annotations

import contextlib
import ctypes
import ctypes.util
import gc
import os
import subprocess
import sys
import threading
from dataclasses import dataclass
from typing import Callable, Iterator, Mapping, MutableMapping, Optional

_MB = 1024 * 1024


class MemoryGuardTripped(RuntimeError):
    """Raised (or passed to a watcher) when a memory limit is crossed."""


@dataclass(frozen=True)
class MemoryLimits:
    max_footprint_mb: int      # this process, including GPU/compressed memory
    min_available_mb: int      # system-wide floor of free + reclaimable memory
    max_swap_growth_mb: int    # swap growth allowed since the guard started


# ---- readers (0 means "this platform cannot tell"; a zero never trips a limit) ----------


def _sysctl_int(name: str) -> int:
    try:
        return int(subprocess.run(["sysctl", "-n", name], capture_output=True, text=True,
                                  timeout=5).stdout.strip())
    except (OSError, ValueError, subprocess.SubprocessError):
        return 0


def _vm_stat_pages() -> dict:
    try:
        out = subprocess.run(["vm_stat"], capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return {}
    pages = {"_size": 16384}
    for line in out.splitlines():
        if "page size of" in line:
            try:
                pages["_size"] = int(line.split("page size of")[1].split()[0])
            except (IndexError, ValueError):
                pass
        elif ":" in line:
            key, _, value = line.partition(":")
            try:
                pages[key.strip()] = int(value.strip().rstrip("."))
            except ValueError:
                pass
    return pages


def _meminfo() -> dict:
    info = {}
    try:
        with open("/proc/meminfo", "r", encoding="ascii") as f:
            for line in f:
                key, _, rest = line.partition(":")
                info[key] = int(rest.split()[0]) / 1024.0      # kB -> MB
    except (OSError, ValueError, IndexError):
        pass
    return info


_proc_lib = None


def process_footprint_mb() -> float:
    """Physical footprint of this process (macOS counts GPU and compressed memory here)."""
    global _proc_lib
    if sys.platform == "darwin":
        try:
            if _proc_lib is None:
                _proc_lib = ctypes.CDLL(ctypes.util.find_library("proc"))
                _proc_lib.proc_pid_rusage.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
            buf = (ctypes.c_uint64 * 64)()
            if _proc_lib.proc_pid_rusage(os.getpid(), 2, ctypes.byref(buf)) == 0:
                return buf[9] / _MB          # rusage_info_v2.ri_phys_footprint
        except (OSError, AttributeError):
            pass
    elif sys.platform.startswith("linux"):
        try:
            with open("/proc/self/status", "r", encoding="ascii") as f:
                fields = {l.split(":")[0]: l.split(":")[1].split()[0] for l in f if ":" in l}
            return (int(fields.get("VmRSS", 0)) + int(fields.get("VmSwap", 0))) / 1024.0
        except (OSError, ValueError, IndexError):
            pass
    try:
        import psutil
        return psutil.Process().memory_info().rss / _MB
    except Exception:  # noqa: BLE001 - optional dependency, any failure means "unknown"
        return 0.0


def available_memory_mb() -> float:
    """Free plus quickly reclaimable memory, system-wide."""
    if sys.platform == "darwin":
        p = _vm_stat_pages()
        size = p.get("_size", 16384)
        pages = sum(p.get(k, 0) for k in ("Pages free", "Pages inactive", "Pages speculative", "Pages purgeable"))
        return pages * size / _MB
    if sys.platform.startswith("linux"):
        return _meminfo().get("MemAvailable", 0.0)
    try:
        import psutil
        return psutil.virtual_memory().available / _MB
    except Exception:  # noqa: BLE001
        return 0.0


def swap_used_mb() -> float:
    if sys.platform == "darwin":
        try:
            out = subprocess.run(["sysctl", "-n", "vm.swapusage"], capture_output=True, text=True,
                                 timeout=5).stdout           # "total = 3584.00M  used = 2273.56M ..."
            return float(out.split("used =")[1].split()[0].rstrip("M"))
        except (OSError, IndexError, ValueError, subprocess.SubprocessError):
            return 0.0
    if sys.platform.startswith("linux"):
        info = _meminfo()
        return max(0.0, info.get("SwapTotal", 0.0) - info.get("SwapFree", 0.0))
    return 0.0


def total_memory_mb() -> float:
    if sys.platform == "darwin":
        return _sysctl_int("hw.memsize") / _MB
    if sys.platform.startswith("linux"):
        return _meminfo().get("MemTotal", 0.0)
    try:
        import psutil
        return psutil.virtual_memory().total / _MB
    except Exception:  # noqa: BLE001
        return 0.0


# ---- limits -----------------------------------------------------------------------------


def _env_int(env: Mapping[str, str], name: str, default: int) -> int:
    try:
        value = int(env[name])
        return value if value > 0 else default
    except (KeyError, ValueError):
        return default


def limits_from_env(total_mb: Optional[float] = None, env: Optional[Mapping[str, str]] = None) -> MemoryLimits:
    """Defaults: 35% of RAM for this process (max 8 GB), 400 MB system floor, 1 GB swap growth."""
    env = os.environ if env is None else env
    total = total_memory_mb() if total_mb is None else total_mb
    default_footprint = min(8192, int(total * 0.35)) if total > 0 else 3072
    return MemoryLimits(
        max_footprint_mb=_env_int(env, "MAGNITE_RAG_MAX_MB", default_footprint),
        min_available_mb=_env_int(env, "MAGNITE_RAG_MIN_FREE_MB", 400),
        max_swap_growth_mb=_env_int(env, "MAGNITE_RAG_MAX_SWAP_MB", 1024),
    )


def preflight(required_mb: float, available_mb: Optional[float] = None, headroom: float = 1.5) -> None:
    """Refuse to start work that clearly will not fit in the memory that is free right now."""
    available = available_memory_mb() if available_mb is None else available_mb
    if available <= 0:
        return                                  # cannot tell on this platform
    needed = required_mb * headroom
    if available < needed:
        raise MemoryGuardTripped(
            f"Not enough free memory: this needs about {required_mb:.0f} MB "
            f"(x{headroom:g} headroom = {needed:.0f} MB) but only {available:.0f} MB is available. "
            "Close other applications or unload the chat model, then retry.")


class MemoryGuard:
    """Checks the three limits; call check() between batches or run watch() in a thread."""

    def __init__(self, limits: MemoryLimits,
                 read_footprint: Callable[[], float] = process_footprint_mb,
                 read_available: Callable[[], float] = available_memory_mb,
                 read_swap: Callable[[], float] = swap_used_mb):
        self.limits = limits
        self._read_footprint = read_footprint
        self._read_available = read_available
        self._read_swap = read_swap
        self._baseline_swap = read_swap()
        self.peak_footprint_mb = 0.0

    def check(self) -> None:
        footprint = self._read_footprint()
        self.peak_footprint_mb = max(self.peak_footprint_mb, footprint)
        lim = self.limits
        if footprint > 0 and footprint > lim.max_footprint_mb:
            raise MemoryGuardTripped(
                f"Embedding stopped: this process's memory footprint reached {footprint:.0f} MB "
                f"(limit {lim.max_footprint_mb} MB, MAGNITE_RAG_MAX_MB).")
        available = self._read_available()
        if available > 0 and available < lim.min_available_mb:
            raise MemoryGuardTripped(
                f"Embedding stopped: only {available:.0f} MB of system memory is available "
                f"(floor {lim.min_available_mb} MB, MAGNITE_RAG_MIN_FREE_MB).")
        swap = self._read_swap()
        if swap > 0 and swap - self._baseline_swap > lim.max_swap_growth_mb:
            raise MemoryGuardTripped(
                f"Embedding stopped: swap grew by {swap - self._baseline_swap:.0f} MB "
                f"(limit {lim.max_swap_growth_mb} MB, MAGNITE_RAG_MAX_SWAP_MB).")

    @contextlib.contextmanager
    def watch(self, on_trip: Callable[[MemoryGuardTripped], None], interval: float = 0.5) -> Iterator[None]:
        """Poll in a daemon thread so growth *inside* a long batch is caught too."""
        stop = threading.Event()

        def loop():
            while not stop.wait(interval):
                try:
                    self.check()
                except MemoryGuardTripped as exc:
                    on_trip(exc)
                    return

        thread = threading.Thread(target=loop, name="rag-memguard", daemon=True)
        thread.start()
        try:
            yield
        finally:
            stop.set()
            thread.join(timeout=2)


# ---- accelerator memory -----------------------------------------------------------------


def apply_mps_limits(env: Optional[MutableMapping[str, str]] = None) -> None:
    """Cap PyTorch's Apple-GPU allocator. Must run before torch is imported; never overrides the user."""
    env = os.environ if env is None else env
    env.setdefault("PYTORCH_MPS_HIGH_WATERMARK_RATIO", "0.5")
    env.setdefault("PYTORCH_MPS_LOW_WATERMARK_RATIO", "0.3")


def release_accelerator_memory() -> None:
    """Return cached GPU memory to the system. Does nothing if torch was never imported."""
    gc.collect()
    torch = sys.modules.get("torch")
    if torch is None:
        return
    try:
        if torch.backends.mps.is_available():
            torch.mps.empty_cache()
    except (AttributeError, RuntimeError):
        pass
    try:
        if torch.cuda.is_available():
            torch.cuda.empty_cache()
    except (AttributeError, RuntimeError):
        pass
