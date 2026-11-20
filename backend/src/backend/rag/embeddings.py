"""Embedding models behind one small abstraction.

* ``EmbedderSpec`` / ``PRESETS``: what each supported model needs (limits, query/document prompts, RAM).
* ``Embedder``: embeds documents in guarded batches and queries with the right prompt; optional
  Matryoshka (MRL) truncation.
* A registry that keeps **one** embedder loaded at a time, unloads the old one before a switch, and
  unloads an idle one after ``MAGNITE_RAG_IDLE_UNLOAD_S`` seconds (default 300, 0 disables). An embedder
  that is in the middle of a call is never unloaded, however long the call takes.
* ``pinned(model)``: a long run (reindex) keeps its target model loaded in a second slot while other
  models are served from the normal slot, so neither side evicts the other. At most two models are
  resident, and only while a pin is held; every load is still preflighted.

``sentence_transformers`` and ``torch`` are imported lazily, inside ``_load_encoder`` and
``_import_torch``, so the base backend starts without the ``[ml]`` extras. Every load and batch goes
through ``memguard`` so an embedding run can never drive the machine into swap.
"""
from __future__ import annotations

import contextlib
import functools
import logging
import os
import platform
import shutil
import sys
import threading
import time
from dataclasses import asdict, dataclass, field
from typing import Callable, Mapping, Optional

import numpy as np

from backend.rag import memguard

logger = logging.getLogger("backend.rag")

MAX_SEQ_TOKENS_CAP = 1024        # chunks are ~500 tokens; this bounds memory for 32k-context models
DEFAULT_BATCH_SIZE = 8
RELEASE_EVERY = 8                # batches between accelerator cache releases
BIG_MACHINE_MB = 15000           # "16 GB" machines report a little under 16384 on Linux
GPU_PRESET_RAM_MB = 1500         # only models at least this heavy are worth the GPU on a big machine

_QWEN_TASK = "Given a web search query, retrieve relevant passages that answer the query"


@dataclass(frozen=True)
class EmbedderSpec:
    model_id: str
    label: str
    max_seq_tokens: int                  # 0 = unknown until the model is loaded
    dim: int                             # 0 = unknown until the model is loaded
    query_prompt: str = ""
    document_prompt: str = ""
    approx_ram_mb: int = 1500            # measured guard footprint, used for the preflight check
    supports_truncation: bool = True     # trained with Matryoshka (MRL) so cutting dims is safe
    trust_remote_code: bool = False
    # Indexing speed measured on REFERENCE_HARDWARE, by device kind ("cpu", "gpu"). Advisory only.
    reference_chunks_per_s: dict = field(default_factory=dict)


MINILM = "sentence-transformers/all-MiniLM-L6-v2"
BGE_M3 = "BAAI/bge-m3"
QWEN3 = "Qwen/Qwen3-Embedding-0.6B"

PRESETS: dict = {
    MINILM: EmbedderSpec(MINILM, "Light", max_seq_tokens=256, dim=384, approx_ram_mb=450,
                         supports_truncation=False, reference_chunks_per_s={"cpu": 40.0}),
    BGE_M3: EmbedderSpec(BGE_M3, "Quality", max_seq_tokens=8192, dim=1024, approx_ram_mb=3000,
                         supports_truncation=False, reference_chunks_per_s={"cpu": 0.2, "gpu": 2.0}),
    QWEN3: EmbedderSpec(QWEN3, "Alternative", max_seq_tokens=32768, dim=1024, approx_ram_mb=3000,
                        query_prompt=f"Instruct: {_QWEN_TASK}\nQuery:", supports_truncation=True,
                        reference_chunks_per_s={"cpu": 0.2, "gpu": 2.0}),
}
REFERENCE_HARDWARE = "Apple M1, 8 GB"   # where reference_chunks_per_s was measured (MiniLM GPU was not)
_ALIASES = {"all-MiniLM-L6-v2": MINILM}      # the id RagConfig has always defaulted to


def spec_for(model_id: str) -> EmbedderSpec:
    """The preset for a known model, else a generic spec (no prompts; limits learned on load)."""
    model_id = _ALIASES.get(model_id, model_id)
    if model_id in PRESETS:
        return PRESETS[model_id]
    return EmbedderSpec(model_id, model_id, max_seq_tokens=0, dim=0)


def list_presets() -> list:
    """Every preset, plus what the UI needs to warn before slow or memory-hungry indexing: reference
    speeds, whether it is this machine's default, and the device kind it would run on."""
    recommended = recommended_model()
    total_mb, cuda, apple_gpu = memguard.total_memory_mb(), _cuda_hint(), _apple_gpu_hint()
    return [{**asdict(spec), "reference_hardware": REFERENCE_HARDWARE,
             "recommended": spec.model_id == recommended,
             "expected_device": planned_device(spec, total_mb, cuda, apple_gpu)}
            for spec in PRESETS.values()]


# ---- hardware policy --------------------------------------------------------------------------


def _import_torch():
    try:
        import torch
        return torch
    except ImportError:
        return None


def _cuda_hint() -> bool:
    """Cheap CUDA detection that does not import torch (only asks it if it is already loaded)."""
    torch = sys.modules.get("torch")
    if torch is not None:
        try:
            return bool(torch.cuda.is_available())
        except (AttributeError, RuntimeError):
            return False
    return bool(shutil.which("nvidia-smi")) or os.path.exists("/proc/driver/nvidia")


def _apple_gpu_hint() -> bool:
    return sys.platform == "darwin" and platform.machine() == "arm64"


def planned_device(spec: EmbedderSpec, total_mb: float, cuda: bool, apple_gpu: bool,
                   env: Optional[Mapping[str, str]] = None) -> str:
    """The device kind ("cpu" or "gpu") ``pick_device`` would choose, without importing torch."""
    env = os.environ if env is None else env
    forced = env.get("MAGNITE_RAG_DEVICE", "").strip().lower()
    if forced:
        return "cpu" if forced == "cpu" else "gpu"
    if cuda or (apple_gpu and spec.approx_ram_mb >= GPU_PRESET_RAM_MB and total_mb >= BIG_MACHINE_MB):
        return "gpu"
    return "cpu"


def default_model(total_mb: float, cuda: bool) -> str:
    """16 GB or more RAM, or a CUDA GPU: the quality model. Otherwise the light one."""
    return BGE_M3 if (cuda or total_mb >= BIG_MACHINE_MB) else MINILM


@functools.lru_cache(maxsize=1)
def recommended_model() -> str:
    return default_model(memguard.total_memory_mb(), _cuda_hint())


def pick_device(spec: EmbedderSpec, total_mb: float, torch_module,
                env: Optional[Mapping[str, str]] = None) -> str:
    """CUDA when present. The Apple GPU only for heavy models on big machines (it costs ~1 GB fixed
    and was no faster for small models); everything else runs on the CPU."""
    env = os.environ if env is None else env
    forced = env.get("MAGNITE_RAG_DEVICE", "").strip().lower()
    if forced:
        return forced
    if torch_module is None:
        return "cpu"
    try:
        if torch_module.cuda.is_available():
            return "cuda"
        if (torch_module.backends.mps.is_available() and spec.approx_ram_mb >= GPU_PRESET_RAM_MB
                and total_mb >= BIG_MACHINE_MB):
            return "mps"
    except (AttributeError, RuntimeError):
        pass
    return "cpu"


def pick_dtype(device: str, torch_module, env: Optional[Mapping[str, str]] = None) -> str:
    env = os.environ if env is None else env
    forced = env.get("MAGNITE_RAG_DTYPE", "").strip().lower()
    if forced in ("float16", "bfloat16", "float32"):
        return forced
    if device == "cuda" and torch_module is not None:
        try:
            return "bfloat16" if torch_module.cuda.is_bf16_supported() else "float16"
        except (AttributeError, RuntimeError):
            return "float16"
    if device == "mps":
        return "float16"
    return "float32"


def is_available() -> bool:
    try:
        import sentence_transformers  # noqa: F401
        return True
    except ImportError:
        return False


def _load_encoder(spec: EmbedderSpec, device: str, dtype: str):
    """The only place sentence-transformers is imported. May download the model."""
    memguard.apply_mps_limits()          # only effective if torch has not been imported yet
    from sentence_transformers import SentenceTransformer

    kwargs = {"trust_remote_code": True} if spec.trust_remote_code else {}
    try:
        model = SentenceTransformer(spec.model_id, device=device, model_kwargs={"dtype": dtype}, **kwargs)
    except TypeError:                    # older transformers spell it torch_dtype
        model = SentenceTransformer(spec.model_id, device=device, model_kwargs={"torch_dtype": dtype}, **kwargs)
    model.max_seq_length = min(model.max_seq_length or MAX_SEQ_TOKENS_CAP, MAX_SEQ_TOKENS_CAP)
    return model


# ---- Embedder ---------------------------------------------------------------------------------


class Embedder:
    def __init__(self, spec: EmbedderSpec, truncate_dim: int = 0, device: Optional[str] = None,
                 dtype: Optional[str] = None, guard_factory: Optional[Callable] = None):
        self.spec = spec
        self.truncate_dim = 0
        self.set_truncate_dim(truncate_dim)
        self.device = device
        self.dtype = dtype
        self._encoder = None
        self._native_dim = 0
        self._busy = 0                   # calls in progress; an embedder in use is never unloaded
        self._unload_pending = False
        self._busy_lock = threading.Lock()
        self._guard_factory = guard_factory or (lambda: memguard.MemoryGuard(memguard.limits_from_env()))

    def set_truncate_dim(self, truncate_dim: int) -> None:
        if truncate_dim and spec_is_known(self.spec) and not self.spec.supports_truncation:
            raise ValueError(f"{self.spec.model_id} does not support embedding truncation; set truncate_dim to 0")
        self.truncate_dim = max(0, int(truncate_dim))

    # -- loading
    @property
    def loaded(self) -> bool:
        return self._encoder is not None

    def _ensure_loaded(self):
        if self._encoder is None:
            torch = _import_torch()
            device = self.device or pick_device(self.spec, memguard.total_memory_mb(), torch)
            dtype = pick_dtype(device, torch)
            self._encoder = _load_encoder(self.spec, device, dtype)
            self.device, self.dtype = device, dtype
        return self._encoder

    @property
    def busy(self) -> bool:
        return self._busy > 0

    @contextlib.contextmanager
    def _in_use(self):
        with self._busy_lock:
            self._busy += 1
        try:
            yield
        finally:
            with self._busy_lock:
                self._busy -= 1
                release = self._busy == 0 and self._unload_pending
            if release:
                self._release()

    def unload(self) -> None:
        """Free the model now, or as soon as the calls running on it finish (the encode loop reloads a
        model that disappears between batches, so freeing it under a running call would only cost a reload)."""
        with self._busy_lock:
            if self._busy:
                self._unload_pending = True
                return
        self._release()

    def _release(self) -> None:
        self._unload_pending = False
        self._encoder = None
        memguard.release_accelerator_memory()

    # -- properties
    @property
    def max_seq_tokens(self) -> Optional[int]:
        declared = self.spec.max_seq_tokens or int(getattr(self._encoder, "max_seq_length", 0) or 0)
        return min(declared, MAX_SEQ_TOKENS_CAP) if declared else None

    @property
    def dim(self) -> int:
        native = self.spec.dim or self._native_dim
        if self.truncate_dim and native:
            return min(native, self.truncate_dim)
        return self.truncate_dim or native

    # -- encoding
    def _encode(self, texts: list, prompt: str) -> np.ndarray:
        encoder = self._ensure_loaded()
        vecs = encoder.encode([prompt + t for t in texts], batch_size=max(1, len(texts)),
                              normalize_embeddings=True, convert_to_numpy=True, show_progress_bar=False)
        vecs = np.asarray(vecs, dtype=np.float32).reshape(len(texts), -1)
        self._native_dim = vecs.shape[1]
        if self.truncate_dim and self.truncate_dim < vecs.shape[1]:
            vecs = vecs[:, :self.truncate_dim]
            norms = np.linalg.norm(vecs, axis=1, keepdims=True)
            vecs = vecs / np.where(norms == 0, 1.0, norms)
        return np.ascontiguousarray(vecs, dtype=np.float32)

    def embed_documents(self, texts: list, batch_size: int = DEFAULT_BATCH_SIZE) -> np.ndarray:
        """(N, dim) float32 unit vectors. Small batches, a memory check after each one."""
        _touch(self)
        if not texts:
            return np.zeros((0, self.dim), dtype=np.float32)
        guard = self._guard_factory()
        parts = []
        with self._in_use():
            for n, start in enumerate(range(0, len(texts), batch_size), 1):
                parts.append(self._encode(list(texts[start:start + batch_size]), self.spec.document_prompt))
                guard.check()
                if n % RELEASE_EVERY == 0:
                    memguard.release_accelerator_memory()
            memguard.release_accelerator_memory()
        _touch(self)
        return np.concatenate(parts, axis=0)

    def embed_query(self, text: str) -> np.ndarray:
        _touch(self)
        with self._in_use():
            vec = self._encode([text], self.spec.query_prompt)[0]
        _touch(self)
        return vec

    def unknown_token_ratio(self, texts: list) -> Optional[float]:
        """Share of these texts' tokens the model's vocabulary does not know (its [UNK] token). None when
        the tokenizer has no unknown token or cannot be asked. A text the model cannot read becomes only
        [UNK] tokens, so every such chunk would get the same vector."""
        tokenizer = getattr(self._encoder, "tokenizer", None)
        unk = getattr(tokenizer, "unk_token_id", None)
        if tokenizer is None or unk is None:
            return None
        try:
            ids = tokenizer(list(texts), add_special_tokens=False, truncation=True,
                            max_length=MAX_SEQ_TOKENS_CAP, verbose=False)["input_ids"]
        except Exception:  # noqa: BLE001 - tokenizers differ; never fail an ingest over a diagnostic
            return None
        total = sum(len(x) for x in ids)
        return sum(x.count(unk) for x in ids) / total if total else 0.0

    def count_tokens(self, texts: list) -> list:
        """Token counts as the model's own tokenizer sees them (word counts if it is not loaded)."""
        tokenizer = getattr(self._encoder, "tokenizer", None)
        if tokenizer is not None:
            try:
                ids = tokenizer(list(texts), add_special_tokens=True, truncation=False, verbose=False)["input_ids"]
                return [len(x) for x in ids]
            except Exception:  # noqa: BLE001 - tokenizers differ; fall back rather than fail an ingest
                pass
        return [len(t.split()) for t in texts]


def spec_is_known(spec: EmbedderSpec) -> bool:
    return spec.model_id in PRESETS


# ---- registry: one model at a time, unload when idle ------------------------------------------

_lock = threading.RLock()
_current: Optional[Embedder] = None
_pinned_id: Optional[str] = None          # model id held by pinned() while a long run is active
_pinned: Optional[Embedder] = None        # its embedder, once loaded (never idles out, never evicted by a query)
_last_used = 0.0
_timer: Optional[threading.Timer] = None


def _idle_seconds() -> float:
    try:
        return float(os.environ.get("MAGNITE_RAG_IDLE_UNLOAD_S", "300"))
    except ValueError:
        return 300.0


def _touch(embedder: Optional[Embedder] = None) -> None:
    global _last_used, _timer
    with _lock:
        if embedder is not None and embedder is not _current:
            return
        _last_used = time.monotonic()
        if _timer is not None:
            _timer.cancel()
            _timer = None
        idle = _idle_seconds()
        if _current is not None and idle > 0:
            _timer = threading.Timer(idle, unload_if_idle)
            _timer.daemon = True
            _timer.start()


def _unload_current() -> None:
    global _current, _timer
    if _timer is not None:
        _timer.cancel()
        _timer = None
    if _current is not None:
        _current.unload()
        _current = None


def get_embedder(model: str, truncate_dim: int = 0) -> Embedder:
    """The loaded embedder for ``model``, switching (unload, preflight, load) when it differs."""
    global _current
    with _lock:
        spec = spec_for(model)
        if _pinned_id is not None and spec.model_id == _pinned_id:
            return _get_pinned(spec, truncate_dim)
        if _current is not None and _current.spec.model_id == spec.model_id:
            _current.set_truncate_dim(truncate_dim)
            _touch(_current)
            return _current
        candidate = Embedder(spec, truncate_dim)          # validates truncate_dim before anything is freed
        _unload_current()
        memguard.preflight(spec.approx_ram_mb)
        candidate._ensure_loaded()
        _current = candidate
        _touch(_current)
        return _current


def _get_pinned(spec: EmbedderSpec, truncate_dim: int) -> Embedder:
    global _pinned
    if _pinned is None:
        candidate = Embedder(spec, truncate_dim)
        memguard.preflight(spec.approx_ram_mb)
        candidate._ensure_loaded()
        _pinned = candidate
    else:
        _pinned.set_truncate_dim(truncate_dim)
    return _pinned


@contextlib.contextmanager
def pinned(model: str):
    """Keep ``model`` loaded for the whole ``with`` block (a reindex), whatever else is asked of the registry.

    Calls for the pinned model use a second slot that nothing evicts or idles out; calls for any other
    model keep using the normal slot. Nothing is loaded here. On success the pinned model becomes the
    normal resident one (the library now uses it); on an error or cancellation it is freed instead.
    """
    global _pinned_id, _pinned, _current, _timer
    model_id = spec_for(model).model_id
    with _lock:
        if _pinned_id is not None:
            raise RuntimeError("Another long embedding run already holds the embedder")
        _pinned_id = model_id
        if _current is not None and _current.spec.model_id == model_id:
            _pinned, _current = _current, None
            if _timer is not None:
                _timer.cancel()
                _timer = None
    try:
        yield
    except BaseException:
        with _lock:
            _pinned_id, held, _pinned = None, _pinned, None
        if held is not None:
            held.unload()
        raise
    else:
        with _lock:
            _pinned_id, held, _pinned = None, _pinned, None
            if held is not None:
                _unload_current()
                _current = held
                _touch(_current)


def _resident(model_id: str) -> Optional[Embedder]:
    """The loaded embedder for this model id, in either slot."""
    for embedder in (_current, _pinned):
        if embedder is not None and embedder.loaded and embedder.spec.model_id == model_id:
            return embedder
    return None


def unload_if_idle(now: Optional[float] = None) -> bool:
    idle = _idle_seconds()
    with _lock:
        if _current is None or idle <= 0 or _current.busy:
            return False
        now = time.monotonic() if now is None else now
        if now - _last_used >= idle:
            _unload_current()
            return True
        return False


def loaded_info(model: Optional[str] = None) -> Optional[dict]:
    """What is loaded: the given model if it is resident in either slot, else the normal slot's model."""
    with _lock:
        if model is not None:
            embedder = _resident(spec_for(model).model_id)
        else:
            embedder = _current if _current is not None and _current.loaded else _resident(_pinned_id or "")
        if embedder is None:
            return None
        return {"model": embedder.spec.model_id, "max_seq_tokens": embedder.max_seq_tokens,
                "dim": embedder.dim, "device": embedder.device, "dtype": embedder.dtype}


# ---- measured speed (in memory only) ----------------------------------------------------------

_measured: dict = {}
_measured_lock = threading.Lock()


def record_speed(model: str, chunks: int, seconds: float, device: Optional[str], dtype: Optional[str]) -> None:
    """Remember how fast a real batched ingest/reindex embedded (latest run wins)."""
    if chunks <= 0 or seconds <= 0:
        return
    with _measured_lock:
        _measured[spec_for(model).model_id] = {"chunks_per_s": chunks / seconds, "device": device, "dtype": dtype}


def measured_speed(model: str) -> Optional[float]:
    with _measured_lock:
        entry = _measured.get(spec_for(model).model_id)
    return entry["chunks_per_s"] if entry else None


def _reset_measured_for_tests() -> None:
    with _measured_lock:
        _measured.clear()


def status_for(model: str, truncate_dim: int = 0) -> dict:
    """Describe the configured embedder without loading it."""
    spec = spec_for(model)
    with _lock:
        loaded = _resident(spec.model_id)
        if loaded is not None:
            return {"model": model, "label": spec.label, "max_seq_tokens": loaded.max_seq_tokens,
                    "dim": loaded.dim, "loaded": True, "device": loaded.device, "dtype": loaded.dtype,
                    "measured_chunks_per_s": measured_speed(model)}
    declared = min(spec.max_seq_tokens, MAX_SEQ_TOKENS_CAP) if spec.max_seq_tokens else None
    dim = min(spec.dim, truncate_dim) if (spec.dim and truncate_dim) else (spec.dim or truncate_dim or None)
    return {"model": model, "label": spec.label, "max_seq_tokens": declared, "dim": dim,
            "loaded": False, "device": None, "dtype": None, "measured_chunks_per_s": measured_speed(model)}


def max_seq_tokens(model: str) -> Optional[int]:
    return status_for(model)["max_seq_tokens"]


def count_tokens(texts: list, model: str) -> Optional[list]:
    """Real token counts when this model is loaded, else None (never loads a model)."""
    with _lock:
        loaded = _resident(spec_for(model).model_id)
        if loaded is not None:
            return loaded.count_tokens(texts)
    return None


def unknown_ratio(texts: list, model: str) -> Optional[float]:
    """``Embedder.unknown_token_ratio`` for the loaded model, else None (never loads a model)."""
    with _lock:
        loaded = _resident(spec_for(model).model_id)
        if loaded is not None:
            return loaded.unknown_token_ratio(texts)
    return None


def _reset_for_tests() -> None:
    global _current, _timer, _pinned, _pinned_id
    with _lock:
        if _timer is not None:
            _timer.cancel()
            _timer = None
        _current = _pinned = _pinned_id = None
    _reset_measured_for_tests()
    clear = getattr(recommended_model, "cache_clear", None)
    if clear is not None:
        clear()


# ---- module-level functions the routes use (and the legacy names) ------------------------------


def embed_documents(texts: list, model: str, truncate_dim: int = 0) -> np.ndarray:
    return get_embedder(model, truncate_dim).embed_documents(texts)


def embed_query(text: str, model: str, truncate_dim: int = 0) -> np.ndarray:
    return get_embedder(model, truncate_dim).embed_query(text)


def embed_texts(texts: list, model: str) -> np.ndarray:
    """Legacy name: embed documents with the model's own settings."""
    if not texts:
        return np.zeros((0, 0), dtype=np.float32)
    return embed_documents(texts, model)


def dimension(model: str, truncate_dim: int = 0) -> int:
    return embed_documents([" "], model, truncate_dim).shape[1]


def cosine_scores(matrix: np.ndarray, query: np.ndarray) -> np.ndarray:
    """Dot product over L2-normalized vectors == cosine similarity."""
    return matrix @ query.reshape(-1)
