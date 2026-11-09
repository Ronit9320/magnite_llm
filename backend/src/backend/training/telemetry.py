"""Read local device telemetry without importing an ML runtime."""
from __future__ import annotations

import csv
import math
import platform
import subprocess


def _number(value: str) -> float | None:
    try:
        result = float(value.strip())
        return result if math.isfinite(result) and result >= 0 else None
    except ValueError:
        return None


def device_telemetry() -> dict:
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
                "detail": "GPU telemetry unavailable. Memory readings describe the local system."}
