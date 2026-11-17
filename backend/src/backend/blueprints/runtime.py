"""Offline Torch/PEFT execution for the Blueprint worker."""

from __future__ import annotations

from dataclasses import dataclass
import hashlib
import json
from pathlib import Path

from backend.blueprints.graph import Graph
from backend.blueprints.schemas import (
    Bindings, GenerationNode, Issue, ModelSnapshot, Problems, ReasoningNode, checked_json,
)
from backend.inference.engines.torch import TorchPeftEngine, messages_to_prompt, torch_device
from backend.inference.supervisor import get_supervisor


def model_messages(node: GenerationNode, inputs: dict) -> list[dict[str, str]]:
    message = next(iter(inputs.values())) if len(inputs) == 1 else inputs
    if not isinstance(message, str):
        message = json.dumps(message, ensure_ascii=False, sort_keys=True)
    messages = [{"role": "user", "content": message}]
    if node.system_prompt:
        messages.insert(0, {"role": "system", "content": node.system_prompt})
    return messages


def encode_answer(node: GenerationNode, outputs: dict) -> str:
    answer = next(iter(outputs.values())) if len(node.outputs) == 1 else outputs
    if node.response == "text":
        if not answer or answer != answer.strip():
            raise ValueError("Text training answers must be nonempty with no surrounding whitespace")
        return answer
    return json.dumps(answer, ensure_ascii=False)


def decode_answer(node: GenerationNode, response: str) -> dict:
    answer = response if node.response == "text" else checked_json(json.loads(response))
    if len(node.outputs) == 1:
        return {next(iter(node.outputs)): answer}
    if not isinstance(answer, dict):
        raise ValueError("Multiple output ports require a JSON object keyed by port")
    return answer


def _weights_size(path: Path) -> int:
    single = path / "model.safetensors"
    if single.is_file() and single.stat().st_size:
        return single.stat().st_size
    index = path / "model.safetensors.index.json"
    if index.is_file():
        shards = {Path(part) for part in json.loads(index.read_text(encoding="utf-8"))["weight_map"].values()}
        if shards and all(not shard.is_absolute() and ".." not in shard.parts for shard in shards):
            sizes = [(path / shard).stat().st_size for shard in shards]
            if all(sizes):
                return sum(sizes)
    raise ValueError("Local Blueprint execution requires a complete Safetensors checkpoint")


def tokenizer_fingerprint(path: Path) -> str:
    digest = hashlib.sha256()
    for name in ("tokenizer_config.json", "tokenizer.json", "tokenizer.model", "spiece.model",
                 "vocab.json", "merges.txt", "special_tokens_map.json", "added_tokens.json",
                 "chat_template.jinja"):
        asset = path / name
        if asset.is_file():
            digest.update(name.encode())
            with asset.open("rb") as handle:
                for chunk in iter(lambda: handle.read(128 * 1024), b""):
                    digest.update(chunk)
    return digest.hexdigest()


@dataclass
class Checkpoint:
    model: ModelSnapshot
    tokenizer: object
    context: int
    weights_bytes: int
    reasoning: bool
    kv_values_per_token: int

    def check(self, node: GenerationNode, device: str, available: int) -> int:
        if isinstance(node, ReasoningNode) and not self.reasoning:
            raise ValueError("The checkpoint's chat template must support explicit thinking mode")
        if node.max_tokens >= self.context:
            raise ValueError(f"Output budget must leave room for input in the {self.context}-token context")
        # CPU inference loads fp32, which can double the bytes of a half-precision
        # checkpoint. Reserve room for runtime allocations and the token budget.
        scalar_bytes = 4 if device == "cpu" else 2
        required = self.weights_bytes * (2 if device == "cpu" else 1) + 256 * 1024**2
        required += node.max_tokens * self.kv_values_per_token * scalar_bytes
        if required > available:
            raise ValueError(f"Approximately {required / 1024**2:.0f} MiB is needed to load this checkpoint; "
                             f"{available / 1024**2:.0f} MiB is available on {device}")
        return required


def check_adapter(checkpoint: Checkpoint, adapter) -> None:
    if adapter is None:
        return
    if adapter.tokenizer_sha256 and adapter.tokenizer_sha256 != checkpoint.model.tokenizer_sha256:
        raise ValueError("The adapter was trained with a different tokenizer")
    config = json.loads((Path(adapter.path) / "adapter_config.json").read_text(encoding="utf-8"))
    if config.get("bias", "none") != "none":
        raise ValueError("Blueprint adapter switching requires LoRA weights trained without shared biases")


def read_checkpoint(model: ModelSnapshot) -> Checkpoint:
    from transformers import AutoConfig, AutoModelForCausalLM, AutoTokenizer

    path = Path(model.path)
    options = {"local_files_only": True, "trust_remote_code": False}
    size = _weights_size(path)
    config = AutoConfig.from_pretrained(model.path, **options)
    if type(config) not in AutoModelForCausalLM._model_mapping:
        raise ValueError("Torch requires a supported decoder-only text checkpoint")
    if getattr(config, "quantization_config", None):
        raise ValueError("Local Blueprint execution requires an unquantized checkpoint")
    tokenizer = AutoTokenizer.from_pretrained(model.path, **options)
    fingerprint = tokenizer_fingerprint(path)
    if model.tokenizer_sha256 and model.tokenizer_sha256 != fingerprint:
        raise ValueError("The tokenizer changed after the run was queued")
    model = model.model_copy(update={"tokenizer": type(tokenizer).__name__,
                                    "tokenizer_sha256": fingerprint, "architecture": config.model_type})
    messages = [{"role": "user", "content": "Test"}]
    reasoning = False
    try:
        thinking = messages_to_prompt(tokenizer, messages, thinking=True)
        plain = tokenizer.apply_chat_template(messages, tokenize=False, add_generation_prompt=True,
                                             enable_thinking=False)
        # The runtime expects native think blocks and a final answer.
        reasoning = thinking != plain and "</think>" in plain
    except (ValueError, TypeError, RuntimeError):
        pass
    context = int(getattr(config, "max_position_embeddings", 2048))
    heads = getattr(config, "num_attention_heads", None) or 1
    kv_heads = getattr(config, "num_key_value_heads", None) or heads
    head_width = getattr(config, "head_dim", None) or getattr(config, "hidden_size", 0) // heads
    kv_values = 2 * kv_heads * head_width * getattr(config, "num_hidden_layers", 0)
    return Checkpoint(model, tokenizer, context, size, reasoning, kv_values)


def device_memory() -> tuple[str, int]:
    import psutil
    import torch

    device = torch_device(torch)
    if device == "cuda":
        return device, torch.cuda.mem_get_info()[0]
    return device, int(psutil.virtual_memory().available)


def check_runtime(graph: Graph, bindings: Bindings) -> dict:
    nodes = [node for node in graph.nodes.values() if isinstance(node, GenerationNode)]
    if not nodes:
        return {"engine": "torch", "device": None, "nodes": {}}

    try:
        if not TorchPeftEngine.is_available():
            raise ValueError("Install Torch, Transformers and PEFT to run locally")
        device, available = device_memory()
    except (ImportError, RuntimeError, OSError, ValueError) as exc:
        raise Problems([Issue(code="runtime_unavailable", message=str(exc))]) from exc

    checkpoints = {}
    hardware = {}
    issues = []
    for node in nodes:
        model = bindings.models[node.id]
        path = "binding.model_id"
        try:
            if model.path not in checkpoints:
                checkpoints[model.path] = read_checkpoint(model)
            checkpoint = checkpoints[model.path]
            required = checkpoint.check(node, device, available)
            path = "binding.adapter_id"
            check_adapter(checkpoint, bindings.adapters.get(node.id))
            bindings.models[node.id] = checkpoint.model.model_copy(update={"id": model.id, "ref": model.ref})
            hardware[node.id] = {"model_id": model.id, "context_tokens": checkpoint.context,
                                 "reasoning": checkpoint.reasoning, "estimated_load_bytes": required}
        except Exception as exc:
            issues.append(Issue(code="unsupported_checkpoint", node_id=node.id, path=path,
                                message=str(exc)))
    if issues:
        raise Problems(issues)
    return {"engine": "torch", "device": device, "available_bytes": available,
            "adapter_switching": True, "nodes": hardware}


class TorchBlueprintSession:
    """Hold one checkpoint between graph steps, with selection and generation serialized."""

    def __init__(self):
        self._engine: TorchPeftEngine | None = None

    def generate(self, node: GenerationNode, inputs: dict, bindings: Bindings, stop) -> dict:
        model = bindings.models[node.id]
        adapter = bindings.adapters.get(node.id)
        try:
            with get_supervisor().operation(wait_for=stop):
                if self._engine is None or self._engine.model_ref != model.path:
                    self.close()
                    device, available = device_memory()
                    read_checkpoint(model).check(node, device, available)
                    self._engine = TorchPeftEngine(model.path, offline_only=True)
                    self._engine.load()
                raw = self._engine.generate_blueprint(
                    model_messages(node, inputs), max_tokens=node.max_tokens,
                    adapter=adapter.path if adapter else "", reasoning=isinstance(node, ReasoningNode), stop=stop,
                )
        except Exception as exc:
            raise Problems([Issue(code="runtime_failed", node_id=node.id, message=str(exc))]) from exc
        if stop.is_set():
            return {}
        try:
            return decode_answer(node, raw)
        except (ValueError, RecursionError) as exc:
            raise Problems([Issue(code="invalid_model_output", node_id=node.id, path="outputs",
                                  message=f"Could not read the model answer: {exc}")]) from exc

    def close(self) -> None:
        if self._engine is not None:
            self._engine.unload()
            self._engine = None
