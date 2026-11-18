from __future__ import annotations

from datetime import datetime
from math import isfinite
from typing import Annotated, Literal

from pydantic import (
    AfterValidator, BaseModel, ConfigDict, Field, JsonValue, StringConstraints,
    model_validator,
)

from backend.textcheck import ensure_utf8


def checked_json(value):
    """Validate JSON values read from requests, dataset files, and Parse JSON."""
    stack = [(value, 0)]
    while stack:
        current, depth = stack.pop()
        if depth > 48:
            raise ValueError("JSON values may be nested at most 48 levels")
        if isinstance(current, str):
            ensure_utf8(current, "JSON string")
        elif isinstance(current, float) and not isfinite(current):
            raise ValueError("JSON numbers must be finite")
        elif isinstance(current, list):
            stack.extend((item, depth + 1) for item in current)
        elif isinstance(current, dict):
            for key in current:
                ensure_utf8(key, "JSON key")
            stack.extend((item, depth + 1) for item in current.values())
    return value


Value = Annotated[JsonValue, AfterValidator(checked_json)]
Text = Annotated[str, AfterValidator(checked_json)]
NodeId = Annotated[str, StringConstraints(pattern=r"^[A-Za-z0-9][A-Za-z0-9_-]{0,63}$")]
PortName = Annotated[str, StringConstraints(pattern=r"^[A-Za-z][A-Za-z0-9_-]{0,63}$")]
RegistryId = Annotated[str, StringConstraints(pattern=r"^[A-Za-z0-9][A-Za-z0-9_.:-]{0,255}$")]
Title = Annotated[Text, StringConstraints(strip_whitespace=True, min_length=1, max_length=200)]
SchemaType = Literal["string", "integer", "number", "boolean", "object", "array", "null"]
SQLITE_MAX_INTEGER = 2**63 - 1


class Contract(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, allow_inf_nan=False)


class ValueSchema(Contract):
    """A deliberately bounded subset of JSON Schema."""

    type: SchemaType
    minimum: int | float | None = None
    minLength: Annotated[int, Field(ge=0)] | None = None
    items: ValueSchema | None = None
    properties: dict[Text, ValueSchema] | None = None
    required: list[Text] | None = None
    additionalProperties: bool | None = None

    @model_validator(mode="after")
    def validate_keywords(self):
        keywords = {
            "string": {"minLength"},
            "integer": {"minimum"},
            "number": {"minimum"},
            "array": {"items"},
            "object": {"properties", "required", "additionalProperties"},
        }
        for keyword in self.model_fields_set - {"type"}:
            if keyword not in keywords.get(self.type, set()) or getattr(self, keyword) is None:
                raise ValueError(f"'{keyword}' is invalid for schema type '{self.type}'")
        if self.required is not None:
            if len(set(self.required)) != len(self.required):
                raise ValueError("'required' contains duplicate properties")
            if set(self.required) - (self.properties or {}).keys():
                raise ValueError("'required' must name declared properties")
        return self


Ports = Annotated[dict[PortName, ValueSchema], Field(max_length=16)]


class Node(Contract):
    id: NodeId
    name: Title | None = None
    inputs: Ports = Field(default_factory=dict)
    outputs: Ports = Field(default_factory=dict)


class InputNode(Node):
    kind: Literal["input"]
    operation: Literal["input"]


class InlineRows(Contract):
    source: Literal["inline"]
    rows: list[Value]


class LocalRows(Contract):
    source: Literal["local"]
    dataset_id: RegistryId


class DatasetNode(Node):
    kind: Literal["dataset"]
    operation: Literal["dataset-rows"]
    binding: InlineRows | LocalRows = Field(discriminator="source")


class TransformNode(Node):
    kind: Literal["transform"]
    operation: Literal["pass", "parse-json", "field", "first", "count", "format",
                       "order-clause", "order-price", "constant", "build-object"]
    parameters: dict[Text, Value] = Field(default_factory=dict)


class ModelBinding(Contract):
    model_id: RegistryId
    adapter_id: RegistryId | None = None


class AdapterNode(Node):
    kind: Literal["adapter"]
    operation: Literal["lora"]
    adapter_id: RegistryId


class GenerationNode(Node):
    binding: ModelBinding
    system_prompt: Annotated[Text, Field(max_length=16000)] = "Return only the requested JSON value."
    response: Literal["json", "text"] = "json"
    max_tokens: Annotated[int, Field(ge=1, le=32768)] = 256


class ModelNode(GenerationNode):
    kind: Literal["model"]
    operation: Literal["generate"]


class ReasoningNode(GenerationNode):
    kind: Literal["reasoning"]
    operation: Literal["reason"]
    max_tokens: Annotated[int, Field(ge=1, le=32768)] = 1024
    max_tokens: Annotated[int, Field(ge=1, le=32768)] = 1024


class OutputNode(Node):
    kind: Literal["output"]
    operation: Literal["output"]


class ToolNode(Node):
    kind: Literal["tool"]
    operation: Literal["approval", "agent"]


class AgentNode(Node):
    kind: Literal["agent"]
    operation: Literal["pi-reply"]
    instructions: Annotated[Text, Field(max_length=16000)] = ""


BlueprintNode = Annotated[
    InputNode | DatasetNode | TransformNode | ModelNode | ReasoningNode | OutputNode | ToolNode
    | AdapterNode | AgentNode,
    Field(discriminator="kind"),
]


class PortRef(Contract):
    node: NodeId
    port: PortName


class Edge(Contract):
    source: PortRef
    target: PortRef


class Blueprint(Contract):
    version: Literal[1]
    name: Title
    nodes: list[BlueprintNode] = Field(min_length=1, max_length=64)
    edges: list[Edge] = Field(default_factory=list, max_length=512)

    @model_validator(mode="before")
    @classmethod
    def strict_version(cls, data):
        if isinstance(data, dict) and "version" in data and type(data["version"]) is not int:
            raise ValueError("Blueprint version must be an integer")
        return data


class Revision(BaseModel):
    id: str
    revision: int
    updated_at: datetime
    blueprint: Blueprint


class BlueprintSummary(BaseModel):
    id: str
    revision: int
    updated_at: datetime
    name: str


class Issue(BaseModel):
    code: str
    node_id: str | None = None
    path: str | None = None
    message: str


class Problems(ValueError):
    def __init__(self, issues: list[Issue]):
        super().__init__(issues[0].message)
        self.issues = issues


class ValidationResult(BaseModel):
    valid: bool
    issues: list[Issue]


class Fixture(Contract):
    outputs: dict[PortName, Value] = Field(max_length=16)
    delay_ms: int = Field(default=0, ge=0, le=5000)


RunMode = Literal["fixture", "local"]
RunSurface = Literal["blueprints", "chat"]


class RunRequest(Contract):
    revision: int | None = Field(default=None, ge=1, le=SQLITE_MAX_INTEGER)
    mode: RunMode = "fixture"
    working_directory: Annotated[Text, Field(min_length=1, max_length=4096)] | None = None
    inputs: dict[NodeId, Value] = Field(default_factory=dict, max_length=64)
    fixtures: dict[NodeId, Fixture] = Field(default_factory=dict, max_length=64)


class NodeTestRequest(Contract):
    name: Title
    node: BlueprintNode
    inputs: dict[PortName, Value] = Field(default_factory=dict, max_length=16)


class DatasetSnapshot(BaseModel):
    id: str
    name: str
    sha256: str
    rows: list[Value]


class ModelSnapshot(BaseModel):
    id: str
    ref: str
    path: str
    tokenizer: str | None = None
    tokenizer_sha256: str | None = None
    architecture: str | None = None


class AdapterSnapshot(BaseModel):
    id: str
    base_model: str
    path: str
    training_job_id: str
    tokenizer_sha256: str | None = None


class Bindings(BaseModel):
    datasets: dict[str, DatasetSnapshot] = Field(default_factory=dict)
    models: dict[NodeId, ModelSnapshot] = Field(default_factory=dict)
    adapters: dict[NodeId, AdapterSnapshot] = Field(default_factory=dict)


RunStatus = Literal["queued", "running", "passed", "failed", "cancelled"]
StepStatus = Literal["pending", "running", "passed", "failed", "cancelled", "skipped"]
ACTIVE_RUNS = ("queued", "running")


class RunSummary(BaseModel):
    id: str
    blueprint_id: str
    revision: int
    name: str
    mode: RunMode = "fixture"
    surface: RunSurface = "blueprints"
    status: RunStatus = "queued"
    created_at: datetime
    started_at: datetime | None = None
    finished_at: datetime | None = None
    error: Issue | None = None
    test_node: NodeId | None = None
    tool_chat: bool | None = None


class StepSummary(BaseModel):
    node_id: str
    status: StepStatus = "pending"
    started_at: datetime | None = None
    finished_at: datetime | None = None
    duration_ms: float | None = None
    error: Issue | None = None


class Step(StepSummary):
    inputs: dict[PortName, Value] = Field(default_factory=dict)
    outputs: dict[PortName, Value] = Field(default_factory=dict)


class RunSnapshot(BaseModel):
    blueprint: Blueprint
    bindings: Bindings
    request: RunRequest
    tool_model: dict[str, str] | None = None
    test_inputs: dict[PortName, Value] | None = None


class Run(RunSummary):
    snapshot: RunSnapshot
    steps: list[Step]
    results: dict[NodeId, Value] = Field(default_factory=dict)
