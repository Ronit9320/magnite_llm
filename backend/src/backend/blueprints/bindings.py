from __future__ import annotations

import hashlib
import json
from pathlib import Path

from backend.adapters.store import AdapterStore
from backend.adapters.validation import validate_weights
from backend.blueprints.graph import Graph, validate_value
from backend.blueprints.schemas import (
    AdapterSnapshot, Bindings, DatasetNode, DatasetSnapshot, Issue, LocalRows,
    ModelNode, ModelSnapshot, Problems, ReasoningNode, checked_json,
)
from backend.models.registry import ModelRegistry
from backend.models.resolution import has_transformer_weights
from backend.training.datasets import DatasetStore

MAX_DATASET_BYTES = 16 * 1024**2


class BindingResolver:
    def __init__(self, models: ModelRegistry, adapters: AdapterStore, datasets: DatasetStore):
        self.models = models
        self.adapters = adapters
        self.datasets = datasets

    def resolve(self, graph: Graph) -> Bindings:
        snapshot = Bindings()
        models: dict[str, ModelSnapshot] = {}
        adapters: dict[str, AdapterSnapshot] = {}
        remaining_bytes = MAX_DATASET_BYTES
        issues = []
        for node in graph.nodes.values():
            path = "binding"
            try:
                if isinstance(node, DatasetNode) and isinstance(node.binding, LocalRows):
                    dataset_id = node.binding.dataset_id
                    path = "binding.dataset_id"
                    if dataset_id not in snapshot.datasets:
                        dataset, size = self._dataset(dataset_id, remaining_bytes)
                        snapshot.datasets[dataset_id] = dataset
                        remaining_bytes -= size
                    validate_value(snapshot.datasets[dataset_id].rows, node.outputs["rows"],
                                   node.id, "binding.rows")
                elif isinstance(node, (ModelNode, ReasoningNode)):
                    model_id = node.binding.model_id
                    path = "binding.model_id"
                    if model_id not in models:
                        models[model_id] = self.model(model_id)
                    model = models[model_id]
                    snapshot.models[node.id] = model
                    adapter_id = node.binding.adapter_id
                    if adapter_id is not None:
                        path = "binding.adapter_id"
                        if adapter_id not in adapters:
                            adapters[adapter_id] = self.adapter(adapter_id)
                        adapter = adapters[adapter_id]
                        self.check_compatible(adapter, model)
                        snapshot.adapters[node.id] = adapter
            except Problems as exc:
                issues.extend(exc.issues)
            except (ValueError, OSError, RecursionError) as exc:
                issues.append(Issue(code="invalid_binding", node_id=node.id, path=path,
                                    message=str(exc)))
        if issues:
            raise Problems(issues)
        return snapshot

    def _dataset(self, dataset_id: str, limit: int) -> tuple[DatasetSnapshot, int]:
        record = self.datasets.acquire(dataset_id)
        try:
            with open(record.path, "rb") as handle:
                content = handle.read(limit + 1)
        finally:
            self.datasets.release(dataset_id)
        if len(content) > limit:
            raise ValueError("Referenced datasets together exceed the 16 MiB snapshot limit")
        rows = []
        # Split on JSONL line boundaries; other Unicode separators may occur inside strings.
        for number, line in enumerate(content.split(b"\n"), 1):
            if line.strip():
                try:
                    rows.append(checked_json(json.loads(line.decode("utf-8"))))
                except (ValueError, RecursionError) as exc:
                    raise ValueError(f"Dataset row {number} is invalid JSON: {exc}") from exc
        if len(rows) != record.rows:
            raise ValueError("Dataset file no longer has its registered row count")
        return DatasetSnapshot(id=record.id, name=record.name, rows=rows,
                               sha256=hashlib.sha256(content).hexdigest()), len(content)

    def model(self, model_id: str) -> ModelSnapshot:
        record = self.models.get_model(model_id)
        if record is None:
            raise ValueError(f"Model '{model_id}' is not registered locally")
        if record.status != "ready" or record.source not in ("huggingface", "local") or not record.path:
            raise ValueError(f"Model '{model_id}' requires a ready local Transformers checkpoint")
        path = Path(record.path).resolve()
        if not path.is_dir() or not has_transformer_weights(path):
            raise ValueError(f"Model '{model_id}' requires a local Transformers checkpoint with weights")
        return ModelSnapshot(id=record.id, ref=record.ref, path=str(path))

    def adapter(self, adapter_id: str) -> AdapterSnapshot:
        record = self.adapters.get(adapter_id)
        if record is None or record.status != "ready" or not record.path:
            raise ValueError(f"Adapter '{adapter_id}' is not ready in the local adapter store")
        path = Path(record.path).resolve()
        validate_weights(path)
        # Locally trained adapters may pin an exact snapshot. Prefer that pin to
        # the registry alias, which can point at a different revision over time.
        base = record.meta.get("resolved_base_model") or record.base_model
        if not isinstance(base, str):
            raise ValueError(f"Adapter '{adapter_id}' has an invalid base model")
        return AdapterSnapshot(id=record.id, base_model=base, path=str(path),
                               training_job_id=record.training_job_id,
                               tokenizer_sha256=record.meta.get("tokenizer_sha256"))

    @staticmethod
    def check_compatible(adapter: AdapterSnapshot, model: ModelSnapshot) -> None:
        if not _matches_model(adapter.base_model, model):
            raise ValueError(f"Adapter '{adapter.id}' was trained for a different base model")


def _matches_model(base: str, model: ModelSnapshot) -> bool:
    if base in (model.id, model.ref, f"hf|{model.ref}"):
        return True
    path = Path(base)
    return path.is_absolute() and path.resolve() == Path(model.path)
