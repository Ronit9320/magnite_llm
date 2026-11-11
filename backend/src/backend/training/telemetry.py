"""Read local device telemetry without importing an ML runtime."""
from __future__ import annotations

import csv
import math
import platform
import subprocess
import time


def _number(value: str) -> float | None:
    try:
        result = float(value.strip())
        return result if math.isfinite(result) and result >= 0 else None
    except ValueError:
        return None


_CPU_MIN_WINDOW_SECONDS = 1.0
_cpu_last_time: float | None = None
_cpu_last_value: float | None = None


def _cpu() -> tuple[float | None, int | None]:
    """System-wide CPU over a window of at least a second.

    Several callers share one counter (Training polls it, Settings reads it), and a window of a few
    milliseconds reads as 0%. Callers inside the window get the last measured value; the very first
    call only sets the baseline, so it is unknown rather than a guess.
    """
    global _cpu_last_time, _cpu_last_value
    try:
        import psutil
        count = psutil.cpu_count(logical=True)
        now = time.monotonic()
        if _cpu_last_time is None:
            psutil.cpu_percent(interval=None)
            _cpu_last_time = now
        elif now - _cpu_last_time >= _CPU_MIN_WINDOW_SECONDS:
            percent = float(psutil.cpu_percent(interval=None))
            _cpu_last_time = now
            _cpu_last_value = percent if math.isfinite(percent) and 0 <= percent <= 100 else None
    except (ImportError, OSError):
        return None, None
    return _cpu_last_value, count


def device_telemetry() -> dict:
    cpu_percent, cpu_count = _cpu()
    data = _device_readings()
    data["cpu_percent"], data["cpu_count"] = cpu_percent, cpu_count
    return data


def _device_readings() -> dict:
    """Report one GPU, never aggregate several devices or infer job allocation."""
    try:
        result = subprocess.run(
            ["nvidia-smi", "--query-gpu=name,memory.used,memory.total,utilization.gpu,temperature.gpu,power.draw",
             "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=2, check=True,
        )
        row = next(csv.reader(result.stdout.splitlines()))
        if len(row) != 6:
            raise ValueError("Unexpected device telemetry")
        used, total, utilization, temperature, power = map(_number, row[1:])
        return {"device": row[0].strip(), "kind": "VRAM", "accelerator": "CUDA",
                "used_bytes": used * 1024**2 if used is not None else None,
                "total_bytes": total * 1024**2 if total is not None else None,
                "utilization_percent": utilization if utilization is not None and utilization <= 100 else None,
                "temperature_c": temperature, "power_w": power,
                "detail": "Device-wide readings. Other processes may also use this GPU."}
    except (OSError, ValueError, StopIteration, subprocess.SubprocessError):
        from backend.training.estimates import memory_budget
        memory = memory_budget()
        total, available = memory.get("total_bytes"), memory.get("available_bytes")
        return {"device": "Local system · " + platform.machine(), "kind": memory["kind"],
                "accelerator": "SYSTEM", "total_bytes": total,
                "used_bytes": max(0, total - available) if total is not None and available is not None else None,
                "utilization_percent": None, "temperature_c": None, "power_w": None,
                "detail": "GPU utilization is not exposed on this system. CPU and memory are system-wide."}
