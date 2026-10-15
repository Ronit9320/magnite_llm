"""Resolve registered identities without silently switching cached revisions."""
import os


def registered_model(ref):
    from backend.routes.models import _get_registry
    record = _get_registry().get_model(ref)
    if record is None:
        raise ValueError(f"Model is no longer registered: {ref}")
    return record


def local_model_path(record):
    if not record.path or not os.path.exists(record.path):
        raise ValueError(f"Registered model files are unavailable: {record.path}")
    return record.path


def resolve_base(ref):
    ref = (ref or "").strip()
    if not ref:
        raise ValueError("base_model is required")
    if ref.startswith("ollama:"):
        raise ValueError("Ollama models cannot be LoRA fine-tuned locally")
    if ref.startswith("huggingface:"):
        return local_model_path(registered_model(ref))
    return ref.removeprefix("hf|")
