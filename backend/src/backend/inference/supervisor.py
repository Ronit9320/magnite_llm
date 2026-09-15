from __future__ import annotations

import os
from typing import Optional

from backend.adapters.schemas import AdapterRecord
from backend.adapters.store import AdapterStore
from backend.inference.engines.base import BaseEngine
from backend.inference.engines.llamacpp import LlamaCppEngine
from backend.inference.engines.torch import TorchPeftEngine
from backend.inference.engines.vllm import VllmEngine
from backend.inference.schemas import EngineInfo, InferenceStatus
from backend.training.trainer import _resolve_base


def detect_gpu() -> tuple[bool, str]:
    """Return (has_usable_gpu, detail). Prefers an NVIDIA GPU via nvidia-smi."""
    if os.name == "nt":
        # On Windows, try WMI/nvidia-smi is unreliable in CI-less envs; assume none.
        return False, "GPU detection not implemented on this platform"
    try:
        import subprocess

        out = subprocess.run(
            ["nvidia-smi", "--query-gpu=name,memory.total", "--format=csv,noheader"],
            capture_output=True, text=True, timeout=5,
        )
        if out.returncode == 0 and out.stdout.strip():
            return True, out.stdout.strip().splitlines()[0].strip()
        return False, "No NVIDIA GPU detected"
    except FileNotFoundError:
        return False, "nvidia-smi not found"
    except Exception as exc:  # noqa: BLE001
        return False, str(exc)


class InferenceSupervisor:
    """Detects hardware and owns the currently-active inference engine."""

    def __init__(self, model_ref: Optional[str] = None, engine_override: int = 0):
        self._engine: Optional[BaseEngine] = None
        self.model_ref = model_ref
        self.engine_override = engine_override
        self._adapter_store: Optional[AdapterStore] = None
        self.active_adapter: Optional[AdapterRecord] = None

        has_gpu, gpu_detail = detect_gpu()
        self.has_gpu = has_gpu
        self.gpu_detail = gpu_detail
        self.llamacpp_ok = LlamaCppEngine.is_available()
        self.vllm_ok = VllmEngine.is_available()
        self.torch_peft_ok = TorchPeftEngine.is_available()

    def set_adapter_store(self, store: AdapterStore) -> None:
        self._adapter_store = store

    def _preferred_engine_name(self) -> str:
        # Override meanings (kept in sync with the C++ Settings panel orders):
        #   0 Auto, 1 vLLM, 2 llama.cpp, 3 Hosted (handled elsewhere in Phase 7)
        if self.engine_override == 1 and self.vllm_ok:
            return "vllm"
        if self.engine_override == 2:
            return "llamacpp"
        if self.has_gpu and self.vllm_ok:
            return "vllm"
        if self.llamacpp_ok:
            return "llamacpp"
        return "llamacpp" if self._engine is not None else "none"

    def available_engines(self, model_ref: str | None = None) -> list[EngineInfo]:
        preferred = self._preferred_engine_name()
        engines = [
            EngineInfo(
                name="vllm",
                available=self.vllm_ok,
                selected=(preferred == "vllm"),
                description="Local GPU serving (requires vLLM)",
                detail=("GPU: " + self.gpu_detail) if (self.vllm_ok and self.has_gpu) else "Not installed",
            ),
            EngineInfo(
                name="llamacpp",
                available=self.llamacpp_ok,
                selected=(preferred == "llamacpp"),
                description="Local serving via llama.cpp (CPU/GPU)",
                detail=("GPU: " + self.gpu_detail) if self.has_gpu else "CPU-only",
            ),
            EngineInfo(
                name="torch",
                available=self.torch_peft_ok,
                selected=False,
                description="Transformers + PEFT serving for LoRA adapters",
                detail="Adapter engine (requires transformers + peft)" if self.torch_peft_ok else "Not installed",
            ),
        ]
        return engines

    def make_engine(self, engine_name: str, model_ref: str) -> BaseEngine:
        if engine_name == "vllm":
            gpu_util = 0.85 if self.has_gpu else 0.0
            return VllmEngine(model_ref, gpu_memory_utilization=gpu_util)
        return LlamaCppEngine(
            model_ref,
            n_gpu_layers=20 if self.has_gpu and self.llamacpp_ok else 0,
        )

    def _resolve_engine(self, requested: str | None, model_ref: str | None = None) -> tuple[str, str]:
        """Return (engine_name, model_ref). Picks sensible defaults."""
        ref = model_ref or self.model_ref
        if requested:
            return requested, (ref or "")
        prefer = self._preferred_engine_name()
        if prefer == "vllm":
            return "vllm", (ref or "")
        return "llamacpp", (ref or "")

    def status(self) -> InferenceStatus:
        available = self.available_engines()
        active_name = self._engine.name if self._engine else None
        active = next((e for e in available if e.name == active_name), None)
        return InferenceStatus(
            engine=active,
            loaded_model=self.model_ref if self._engine else None,
            model_source="",
            backend_available=True,
            active_adapter=self.active_adapter.name if self.active_adapter else None,
            active_adapter_id=self.active_adapter.id if self.active_adapter else None,
        )

    def load_model(self, model_ref: str, engine_name: str | None = None) -> None:
        """Load (or swap) a model into the active engine. Clears any active adapter."""
        ename, mref = self._resolve_engine(engine_name, model_ref)
        if not mref:
            raise ValueError("No model selected for inference")
        self.model_ref = mref

        # Unload any existing engine/model (also clears the active adapter).
        self.unload()

        engine = self.make_engine(ename, mref)
        engine.load()
        self._engine = engine

    def activate_adapter(self, adapter_id: str) -> AdapterRecord:
        """Activate a trained adapter for inference, restarting the engine.

        The adapter is loaded on top of its base model via transformers + PEFT
        (the 'torch' engine). Any currently-loaded plain model is unloaded first.
        """
        if self._adapter_store is None:
            raise RuntimeError("Adapter store not initialized")
        rec = self._adapter_store.get(adapter_id)
        if rec is None:
            raise ValueError(f"Unknown adapter '{adapter_id}'")
        if rec.status != "ready" or not rec.path or not os.path.isdir(rec.path):
            raise ValueError(
                f"Adapter '{rec.name}' cannot be used (missing files on disk)"
            )
        if not self.torch_peft_ok:
            raise RuntimeError(
                "Adapter inference needs transformers + peft. Install with "
                "'uv pip install --python .venv/bin/python -e 'backend[ml]''."
            )
        base = _resolve_base(rec.base_model)

        self.unload()
        engine = TorchPeftEngine(base, rec.path)
        engine.load()
        self._engine = engine
        self.model_ref = base
        self.active_adapter = rec
        return rec

    def deactivate_adapter(self) -> None:
        """Clear the active adapter and unload the adapter engine."""
        self.unload()

    def chat_stream(self, messages, **kwargs):
        if self._engine is None:
            raise RuntimeError("No inference engine loaded")
        return self._engine.chat_stream(messages, **kwargs)

    def complete_stream(self, prompt, **kwargs):
        if self._engine is None:
            raise RuntimeError("No inference engine loaded")
        return self._engine.complete_stream(prompt, **kwargs)

    def unload(self) -> None:
        if self._engine is not None:
            try:
                self._engine.unload()
            finally:
                self._engine = None
        self.active_adapter = None


_supervisor: InferenceSupervisor | None = None


def get_supervisor() -> InferenceSupervisor:
    global _supervisor
    if _supervisor is None:
        _supervisor = InferenceSupervisor()
    return _supervisor
