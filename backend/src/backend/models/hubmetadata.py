"""Read-only Hub metadata used by the native model discovery workspace."""
from __future__ import annotations

import json
import math
from pathlib import Path

import httpx
from huggingface_hub import hf_hub_download


def _positive_number(value):
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        return None
    return int(value) if math.isfinite(value) and 0 < value <= 10**18 and int(value) == value else None


def _strings(value) -> list[str]:
    if isinstance(value, str):
        return [value] if value else []
    return [item for item in value if isinstance(item, str)] if isinstance(value, list) else []


def search_metadata(info) -> dict:
    """Search may omit config/card fields. Omitted measurements stay unknown."""
    card = getattr(info, "cardData", None) or {}
    if hasattr(card, "to_dict"):
        card = card.to_dict()
    if not isinstance(card, dict):
        card = {}
    config = getattr(info, "config", None) or {}
    if not isinstance(config, dict):
        config = {}
    tags = list(getattr(info, "tags", None) or [])
    tensors = getattr(info, "safetensors", None)
    parameters = getattr(tensors, "total", None)
    if isinstance(tensors, dict):
        parameters = tensors.get("total")
    gguf = getattr(info, "gguf", None) or {}
    if not isinstance(gguf, dict):
        gguf = {}
    if parameters is None:
        parameters = gguf.get("total")
    architectures = _strings(config.get("architectures"))
    formats = [name for name in ("gguf", "safetensors", "pytorch") if name in tags]
    if gguf and "gguf" not in formats:
        formats.append("gguf")
    license_value = card.get("license", "")
    if not isinstance(license_value, str):
        license_value = ", ".join(_strings(license_value))
    if not license_value:
        license_value = next((tag[8:] for tag in tags if tag.startswith("license:")), "")
    return {
        "pipeline_tag": getattr(info, "pipeline_tag", None) or "",
        "library_name": getattr(info, "library_name", None) or "",
        "architecture": config.get("model_type") or gguf.get("architecture") or (architectures[0] if architectures else ""),
        "parameters": _positive_number(parameters),
        "context_length": _positive_number(config.get("max_position_embeddings") or config.get("n_positions") or gguf.get("context_length")),
        "languages": _strings(card.get("language")),
        "formats": formats,
        "license": license_value,
    }


def model_details(api, repo_id: str, cache_dir: str) -> dict:
    info = api.model_info(repo_id, files_metadata=True, timeout=12)
    metadata = search_metadata(info)
    files = list(getattr(info, "siblings", None) or [])
    revision = getattr(info, "sha", None) or ""
    config = {}
    config_note = "Configuration unavailable from the publisher."
    config_file = next((item for item in files if item.rfilename == "config.json"), None)
    # Fetch only a bounded JSON configuration, pinned to the metadata revision.
    # No model weights or executable publisher code are read here.
    if config_file is not None and config_file.size is not None and 0 < config_file.size <= 1_048_576 and revision:
        try:
            path = hf_hub_download(repo_id, "config.json", revision=revision,
                                   cache_dir=cache_dir, etag_timeout=8)
            with Path(path).open("rb") as stream:
                raw = stream.read(1_048_577)
            parsed = json.loads(raw) if len(raw) <= 1_048_576 else None
            if isinstance(parsed, dict):
                config = parsed
                config_note = ""
        except (httpx.HTTPError, OSError, ValueError, RuntimeError):
            config_note = "Configuration could not be read. Other metadata is still available."
    architecture = config.get("model_type")
    if isinstance(architecture, str):
        metadata["architecture"] = architecture
    context = _positive_number(config.get("max_position_embeddings") or config.get("n_positions"))
    if context:
        metadata["context_length"] = context
    known_sizes = [item.size for item in files if item.size is not None and item.size >= 0]
    total_bytes = sum(known_sizes) if files and len(known_sizes) == len(files) else None
    ordered_files = sorted(files, key=lambda item: (
        not item.rfilename.lower().endswith((".gguf", ".safetensors", ".bin")), item.rfilename))
    file_list = [{"path": item.rfilename, "size_bytes": item.size} for item in ordered_files[:250]]
    for item in files:
        suffix = item.rfilename.lower().rsplit(".", 1)[-1]
        if suffix in ("gguf", "safetensors") and suffix not in metadata["formats"]:
            metadata["formats"].append(suffix)
    card = getattr(info, "cardData", None) or {}
    if hasattr(card, "to_dict"):
        card = card.to_dict()
    if not isinstance(card, dict):
        card = {}
    evaluations = []
    model_index = card.get("model-index", [])
    for model in model_index if isinstance(model_index, list) else []:
        if not isinstance(model, dict):
            continue
        results = model.get("results", [])
        for result in results if isinstance(results, list) else []:
            if not isinstance(result, dict):
                continue
            dataset = result.get("dataset") or {}
            if not isinstance(dataset, dict):
                dataset = {}
            metrics = result.get("metrics", [])
            for metric in metrics if isinstance(metrics, list) else []:
                if not isinstance(metric, dict):
                    continue
                value = metric.get("value")
                if isinstance(value, (str, int, float)) and not isinstance(value, bool):
                    evaluations.append({"dataset": str(dataset.get("name", dataset.get("type", "Unspecified"))),
                                        "metric": str(metric.get("name", metric.get("type", "Score"))),
                                        "value": str(value)})
    return {
        **metadata, "repo_id": repo_id, "revision": revision,
        "gated": bool(getattr(info, "gated", False)),
        "downloads": getattr(info, "downloads", None) or 0,
        "likes": getattr(info, "likes", None) or 0,
        "files": file_list, "file_count": len(files), "total_bytes": total_bytes,
        "config_note": config_note,
        "layers": _positive_number(config.get("num_hidden_layers") or config.get("n_layer")),
        "hidden_size": _positive_number(config.get("hidden_size") or config.get("n_embd")),
        "attention_heads": _positive_number(config.get("num_attention_heads") or config.get("n_head")),
        "kv_heads": _positive_number(config.get("num_key_value_heads")),
        "head_dim": _positive_number(config.get("head_dim")),
        "evaluations": evaluations[:60],
    }
