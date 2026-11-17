"""Resolve registered identities without silently switching cached revisions."""
import json
import os
from pathlib import Path

from huggingface_hub import snapshot_download


def has_transformer_weights(path: Path) -> bool:
    """Recognize a local Transformers checkpoint without importing ML."""
    if not (path / "config.json").is_file():
        return False
    for name in ("model.safetensors", "pytorch_model.bin"):
        weight = path / name
        if weight.is_file() and weight.stat().st_size > 0:
            return True
    for name in ("model.safetensors.index.json", "pytorch_model.bin.index.json"):
        try:
            index = json.loads((path / name).read_text(encoding="utf-8"))
            shards = set(index["weight_map"].values())
            if shards and all(
                not Path(shard).is_absolute() and ".." not in Path(shard).parts
                for shard in shards
            ) and all((path / shard).is_file() and (path / shard).stat().st_size > 0
                      for shard in shards):
                return True
        except (OSError, ValueError, KeyError, TypeError, AttributeError):
            continue
    return False


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
    if ref.startswith(("huggingface:", "local:")):
        return local_model_path(registered_model(ref))
    return ref.removeprefix("hf|")


def resolve_base_offline(ref):
    """Like resolve_base, but never touches the network.

    A bare Hub repo id is accepted only if a snapshot of it is already in the local cache.
    Adapters can arrive in an imported archive, so the base model they name is untrusted:
    activating one must not make the backend fetch a model of the archive's choosing.
    """
    base = resolve_base(ref)
    if os.path.isdir(base):
        return base
    try:
        return snapshot_download(base, local_files_only=True)
    except Exception as exc:  # LocalEntryNotFoundError, HFValidationError, OSError, ...
        raise ValueError(f"Adapter base model is not downloaded: {ref}") from exc
