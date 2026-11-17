"""Adapt typed Blueprint examples to the existing LoRA training queue."""

from __future__ import annotations

import json

from pydantic import Field, model_validator

from backend.blueprints.graph import validate_ports
from backend.blueprints.runtime import encode_answer, model_messages, read_checkpoint
from backend.blueprints.schemas import (
    SQLITE_MAX_INTEGER, Contract, GenerationNode, PortName, RegistryId, Revision, Value,
)
from backend.inference.engines.torch import messages_to_prompt
from backend.training.schemas import BlueprintTrainingOrigin, TrainingConfig, TrainingOptions


class NodeExample(Contract):
    inputs: dict[PortName, Value] = Field(max_length=16)
    outputs: dict[PortName, Value] = Field(max_length=16)


class NodeTrainingRequest(Contract):
    revision: int | None = Field(default=None, ge=1, le=SQLITE_MAX_INTEGER)
    examples: list[NodeExample] = Field(min_length=1, max_length=5000)
    settings: TrainingOptions = Field(default_factory=TrainingOptions)


class NodeAdapterRequest(Contract):
    revision: int = Field(ge=1, le=SQLITE_MAX_INTEGER)
    adapter_id: RegistryId | None = None
    training_job_id: RegistryId | None = None

    @model_validator(mode="after")
    def one_source(self):
        if (self.adapter_id is None) == (self.training_job_id is None):
            raise ValueError("Provide an adapter_id or a training_job_id")
        return self


def example_tokens(tokenizer, messages: list[dict]) -> tuple[list[int], list[int]]:
    """Mask the prompt labels when training a node's final answer."""
    if len(messages) < 2 or messages[-1].get("role") != "assistant":
        raise ValueError("Blueprint examples must end with an assistant response")
    prompt = messages_to_prompt(tokenizer, messages[:-1], thinking=False)
    prefix = tokenizer.encode(prompt, add_special_tokens=False)
    answer = tokenizer.encode(messages[-1]["content"], add_special_tokens=False)
    if not answer or tokenizer.eos_token_id is None:
        raise ValueError("Blueprint examples require an answer and a tokenizer EOS token")
    answer.append(tokenizer.eos_token_id)
    return prefix + answer, [-100] * len(prefix) + answer


def start_node_training(resolver, manager, revision: Revision, node: GenerationNode,
                        request: NodeTrainingRequest):
    model = resolver.model(node.binding.model_id)
    checkpoint = read_checkpoint(model)
    rows = []
    for index, example in enumerate(request.examples):
        path = f"examples[{index}]"
        validate_ports(example.inputs, node.inputs, node.id, f"{path}.inputs")
        validate_ports(example.outputs, node.outputs, node.id, f"{path}.outputs")
        messages = model_messages(node, example.inputs)
        messages.append({"role": "assistant", "content": encode_answer(node, example.outputs)})
        tokens, labels = example_tokens(checkpoint.tokenizer, messages)
        answer_tokens = sum(label != -100 for label in labels) - 1
        if answer_tokens > node.max_tokens:
            raise ValueError(f"{path} needs {answer_tokens} output tokens; increase the node's token budget")
        limit = min(request.settings.max_length, checkpoint.context)
        if len(tokens) > limit:
            raise ValueError(f"{path} needs {len(tokens)} training tokens; limit is {limit}")
        rows.append(json.dumps({"messages": messages}, ensure_ascii=False))
    name = request.settings.name.strip() or f"{revision.blueprint.name} · {node.name or node.id}"
    dataset = resolver.datasets.ingest_content(name, "\n".join(rows), fmt="jsonl", source="blueprint")
    origin = BlueprintTrainingOrigin(blueprint_id=revision.id, revision=revision.revision,
                                     model_id=model.id, node_id=node.id,
                                     tokenizer_sha256=checkpoint.model.tokenizer_sha256)
    config = TrainingConfig(**request.settings.model_dump(exclude={"name"}), name=name,
                            base_model=model.path, dataset_id=dataset.id, blueprint_node=origin)
    try:
        return manager.start(config)
    except Exception:
        resolver.datasets.delete(dataset.id)
        raise
