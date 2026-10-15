from __future__ import annotations

import os
import sys
import platform
import threading
import anyio
from contextlib import contextmanager, aclosing
from pathlib import Path
from typing import Optional

from backend.adapters.schemas import AdapterRecord
from backend.adapters.store import AdapterStore
from backend.inference.engines.base import BaseEngine
from backend.inference.engines.llamacpp import LlamaCppEngine
from backend.inference.engines.torch import TorchPeftEngine
from backend.inference.engines.vllm import VllmEngine
from backend.inference.engines.ollama import OllamaEngine
from backend.inference.engines.streaming import run_blocking
from backend.inference.schemas import EngineInfo, InferenceStatus
from backend.training.trainer import _resolve_base


def detect_gpu() -> tuple[bool, str]:
    """Return (has_usable_gpu, detail). Prefers an NVIDIA GPU via nvidia-smi."""
    if sys.platform == "darwin" and platform.machine().lower() in ("arm64", "aarch64"):
        return True, "Apple Silicon (Metal/MPS)"
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
        self._operation = threading.Lock()
        self._engine: Optional[BaseEngine] = None
        self.model_ref = model_ref
        self.engine_override = int(os.environ.get("MAGNITE_ENGINE_OVERRIDE", engine_override))
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
        if self.engine_override == 1:
            return "vllm"
        if self.engine_override == 2:
            return "llamacpp"
        if self.has_gpu and self.vllm_ok and sys.platform != "darwin":
            return "vllm"
        if self.llamacpp_ok:
            return "llamacpp"
        return "llamacpp" if self._engine is not None else "none"

    def available_engines(self, model_ref: str | None = None) -> list[EngineInfo]:
        preferred = self._preferred_engine_name()
        engines = [
            EngineInfo(name="ollama", available=True, selected=False,
                       description="Local Ollama server", detail="Requires a running Ollama server"),
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
        if engine_name == "torch":
            return TorchPeftEngine(model_ref)
        if engine_name == "ollama":
            return OllamaEngine(model_ref)
        if engine_name == "vllm":
            gpu_util = 0.85 if self.has_gpu else 0.0
            return VllmEngine(model_ref, gpu_memory_utilization=gpu_util)
        if engine_name != "llamacpp":
            raise ValueError(f"Unsupported inference engine: {engine_name}")
        return LlamaCppEngine(
            model_ref,
            n_gpu_layers=20 if self.has_gpu and self.llamacpp_ok else 0,
        )

    def _resolve_engine(self, requested: str | None, model_ref: str | None = None) -> tuple[str, str]:
        """Return (engine_name, model_ref). Picks sensible defaults."""
        ref = model_ref or self.model_ref
        if requested and requested not in ("torch", "llamacpp", "vllm", "ollama"):
            raise ValueError(f"Unsupported inference engine: {requested}")
        if ref and ref.startswith(("huggingface:", "ollama:")):
            from backend.routes.models import _get_registry
            record = _get_registry().get_model(ref)
            if record is None:
                raise ValueError(f"Model is no longer registered: {ref}")
            if record.source == "ollama":
                if requested and requested != "ollama":
                    raise ValueError("Ollama models must use Auto or the Ollama engine")
                return "ollama", record.ref
            from backend.models.resolution import local_model_path
            ref = local_model_path(record)
        if ref and os.path.isdir(ref) and not requested and not self.engine_override:
            if any(Path(ref).rglob("*.gguf")):
                return "llamacpp", ref
            return ("vllm" if self.has_gpu and self.vllm_ok else "torch"), ref
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

    def load_model(self, model_ref: str, engine_name: str | None = None, cancelled=None) -> None:
        with self.operation():
            self._load_model(model_ref, engine_name, cancelled)

    def _load_model(self, model_ref: str, engine_name: str | None = None, cancelled=None) -> None:
        """Load (or swap) a model into the active engine. Clears any active adapter."""
        ename, mref = self._resolve_engine(engine_name, model_ref)
        if not mref:
            raise ValueError("No model selected for inference")
        # Unload any existing engine/model (also clears the active adapter).
        self._unload()

        engine = self.make_engine(ename, mref)
        try:
            engine.cancel_load = cancelled
            if cancelled is not None and cancelled.is_set():
                raise RuntimeError("Model loading cancelled")
            engine.load()
            if cancelled is not None and cancelled.is_set():
                raise RuntimeError("Model loading cancelled")
        except Exception:
            engine.unload()
            raise
        self._engine = engine
        self.model_ref = mref

    def activate_adapter(self, adapter_id: str, cancelled=None) -> AdapterRecord:
        with self.operation():
            return self._activate_adapter(adapter_id, cancelled)

    def _activate_adapter(self, adapter_id: str, cancelled=None) -> AdapterRecord:
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
        base = rec.meta.get("resolved_base_model") or _resolve_base(rec.base_model)
        if os.path.isabs(base) and not os.path.isdir(base):
            raise ValueError(f"Adapter base snapshot is unavailable: {base}")

        self._unload()
        engine = TorchPeftEngine(base, rec.path)
        try:
            if cancelled is not None and cancelled.is_set():
                raise RuntimeError("Adapter loading cancelled")
            engine.load()
            if cancelled is not None and cancelled.is_set():
                raise RuntimeError("Adapter loading cancelled")
        except Exception:
            engine.unload()
            raise
        self._engine = engine
        self.model_ref = base
        self.active_adapter = rec
        return rec

    def deactivate_adapter(self) -> None:
        """Clear the active adapter and unload the adapter engine."""
        self.unload()

    def chat_stream(self, messages, **kwargs):
        return self._stream("chat_stream", messages, **kwargs)

    def complete_stream(self, prompt, **kwargs):
        return self._stream("complete_stream", prompt, **kwargs)

    @contextmanager
    def operation(self):
        if not self._operation.acquire(blocking=False):
            raise RuntimeError("Inference is busy. Stop generation or wait for model loading to finish.")
        try:
            yield
        finally:
            self._operation.release()

    async def _stream(self, method, data, **kwargs):
        with self.operation():
            if self._engine is None:
                raise RuntimeError("No inference engine loaded. Load a model first.")
            prepare = kwargs.pop("prepare", None)
            if prepare:
                with anyio.CancelScope(shield=True):
                    data = await run_blocking(prepare, self._engine)
            async with aclosing(getattr(self._engine, method)(data, **kwargs)) as stream:
                async for text in stream:
                    yield text

    def unload(self) -> None:
        with self.operation():
            self._unload()

    def _unload(self) -> None:
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
