"""Local, advisory training budgets. Never load weights or import ML runtimes."""
from __future__ import annotations

import json
import math
from pathlib import Path
import subprocess
import sys

from backend.training.schemas import TrainingConfig


def memory_budget() -> dict:
    """Probe free NVIDIA memory, otherwise report system/unified memory."""
    try:
        result = subprocess.run(
            ["nvidia-smi", "--query-gpu=memory.free,memory.total", "--format=csv,noheader,nounits"],
            capture_output=True, text=True, timeout=2, check=True)
        # The trainer uses the default device, not the aggregate of all GPUs.
        free, total = (float(v.strip()) * 1024**2 for v in result.stdout.splitlines()[0].split(","))
        return {"kind": "VRAM", "available_bytes": free, "total_bytes": total}
    except (OSError, ValueError, IndexError, subprocess.SubprocessError):
        pass
    try:
        if sys.platform == "win32":
            import ctypes
            class MemoryStatus(ctypes.Structure):
                _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong)] + [
                    (name, ctypes.c_ulonglong) for name in
                    ("total", "available", "page_total", "page_available", "virtual_total", "virtual_available", "extended")]
            status = MemoryStatus()
            status.length = ctypes.sizeof(status)
            if not ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(status)):
                raise OSError("Memory probe failed")
            return {"kind": "System RAM", "available_bytes": status.available, "total_bytes": status.total}
        if sys.platform == "darwin":
            total = int(subprocess.check_output(["sysctl", "-n", "hw.memsize"], timeout=2))
            return {"kind": "Unified/system RAM", "available_bytes": None, "total_bytes": total}
        values = dict(line.split(":", 1) for line in Path("/proc/meminfo").read_text().splitlines())
        return {"kind": "System RAM", "available_bytes": int(values["MemAvailable"].split()[0])*1024,
                "total_bytes": int(values["MemTotal"].split()[0])*1024}
    except (OSError, ValueError, KeyError, subprocess.SubprocessError):
        return {"kind": "Memory", "available_bytes": None, "total_bytes": None}


def local_config(ref: str) -> dict:
    from backend.models.resolution import resolve_base
    base = Path(resolve_base(ref))
    if not base.is_dir():
        from huggingface_hub import try_to_load_from_cache
        cached = try_to_load_from_cache(ref.removeprefix("hf|"), "config.json")
        if not isinstance(cached, str):
            raise ValueError("Choose cached Transformers weights to estimate resources")
        base = Path(cached).parent
    path = base / "config.json"
    if path.stat().st_size > 1024*1024:
        raise ValueError("Model configuration is too large")
    config = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(config, dict):
        raise ValueError("Model configuration must be an object")
    return config


def estimate(cfg: TrainingConfig, rows: int, jobs=()) -> dict:
    result = {"memory": memory_budget(), "memory_low_bytes": None, "memory_high_bytes": None,
              "adapter_bytes": None, "duration_low_seconds": None, "duration_high_seconds": None,
              "steps": None, "assumptions": "Advisory architecture budget; allocator, kernels and padding can change actual use."}
    if rows > 0:
        train_rows = rows - (max(1, math.ceil(rows*cfg.validation_split)) if cfg.validation_split else 0)
        result["steps"] = cfg.max_steps or math.ceil(max(1, train_rows)/cfg.batch_size)*cfg.epochs
    try:
        config = local_config(cfg.base_model)
        kind = config.get("model_type", "")
        if kind not in ("llama", "mistral", "qwen2", "qwen3", "gemma", "gemma2", "gpt2"):
            raise ValueError("Resource budgeting is unavailable for this model architecture")
        h = int(config.get("hidden_size", config.get("n_embd", 0)))
        layers = int(config.get("num_hidden_layers", config.get("n_layer", 0)))
        vocab = int(config.get("vocab_size", 0))
        intermediate = int(config.get("intermediate_size", config.get("n_inner") or 4*h))
        heads = int(config.get("num_attention_heads", config.get("n_head", 1)))
        kv = int(config.get("num_key_value_heads", heads))
        if min(h, layers, vocab, intermediate, heads, kv) <= 0:
            raise ValueError("Model dimensions unavailable")
        head_dim = int(config.get("head_dim", h//heads))
        q = heads*head_dim
        k = kv*head_dim
        # Projection matrices used to budget the base and low-rank weights.
        projections = {"q_proj": (h,q), "k_proj": (h,k), "v_proj": (h,k), "o_proj": (q,h),
                       "gate_proj": (h,intermediate), "up_proj": (h,intermediate), "down_proj": (intermediate,h)}
        if kind == "gpt2":
            projections = {"c_attn": (h,3*h), "c_proj_attn": (h,h), "c_fc": (h,intermediate), "c_proj_mlp": (intermediate,h)}
        params = layers*sum(a*b for a,b in projections.values())
        params += vocab*h*(1 if config.get("tie_word_embeddings", kind=="gpt2") else 2)
        if kind == "gpt2":
            params += int(config.get("n_positions", 1024))*h
        targets = [v.strip() for v in cfg.target_modules.split(",")]
        chosen = [dims for key,dims in projections.items() if any(t in key for t in targets)]
        if cfg.target_modules == "all-linear" or not chosen:
            chosen = list(projections.values())
        adapter = layers*sum(cfg.rank*(a+b) for a,b in chosen)
        weight_bytes = {"none": 4.0, "4bit": 0.65, "8bit": 1.25}[cfg.quantization]
        base = params*weight_bytes + adapter*16  # weights, gradients, optimizer budget
        activations = cfg.batch_size*cfg.max_length*h*layers*4
        result.update(memory_low_bytes=base+activations*4+256*1024**2,
                      memory_high_bytes=base*1.3+activations*16+cfg.batch_size*heads*cfg.max_length**2*layers*4+1024**3,
                      adapter_bytes=adapter*4)
        result["assumptions"] += " FP32 base for LoRA; quantized base with overhead for QLoRA. Adapter size assumes FP32 weights."
    except (OSError, ValueError, TypeError, KeyError, RuntimeError) as exc:
        result["detail"] = str(exc)
    # Duration is calibrated only from actual matching completed runs on this installation.
    for job in jobs:
        prior = job.config
        if job.status != "done" or job.base_model != cfg.base_model:
            continue
        if any(prior.get(key) != getattr(cfg,key) for key in ("batch_size", "max_length", "quantization", "rank", "target_modules")):
            continue
        timed = [p for p in job.history if "loss" in p and "elapsed_seconds" in p]
        if len(timed) < 2 or not result["steps"]:
            continue
        steps = timed[-1]["step"] - timed[0]["step"]
        seconds = timed[-1]["elapsed_seconds"] - timed[0]["elapsed_seconds"]
        if steps > 0 and seconds > 0 and math.isfinite(seconds):
            duration = result["steps"]*seconds/steps
            result.update(duration_low_seconds=duration*0.7, duration_high_seconds=duration*1.5)
            result["assumptions"] += " Duration uses a matching local run; excludes initial loading and final saving."
            break
    return result
