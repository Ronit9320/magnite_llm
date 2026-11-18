from __future__ import annotations

from collections import deque
from dataclasses import dataclass

from backend.blueprints.schemas import (
    Blueprint, BlueprintNode, DatasetNode, GenerationNode, InlineRows, Issue,
    Problems, RunRequest, ToolNode, TransformNode, ValueSchema,
)


@dataclass
class Graph:
    nodes: dict[str, BlueprintNode]
    order: list[str]
    sources: dict[str, dict[str, tuple[str, str]]]


def _compatible(source: ValueSchema, target: ValueSchema) -> bool:
    if source.type != target.type:
        return source.type == "integer" and target.type == "number"
    if source.items and target.items:
        return _compatible(source.items, target.items)
    if source.properties and target.properties:
        return all(_compatible(source.properties[key], target.properties[key])
                   for key in source.properties.keys() & target.properties.keys())
    return True


def _signature(issues, node, inputs, outputs):
    for direction, expected in (("inputs", inputs), ("outputs", outputs)):
        ports = getattr(node, direction)
        if ports.keys() != expected.keys():
            names = ", ".join(expected) or "none"
            issues.append(Issue(code="invalid_ports", node_id=node.id, path=direction,
                                message=f"Expected {direction}: {names}"))
            continue
        for name, expected_type in expected.items():
            if expected_type is not None and ports[name].type != expected_type:
                issues.append(Issue(code="invalid_ports", node_id=node.id,
                                    path=f"{direction}.{name}",
                                    message=f"Port must have type '{expected_type}'"))


_TRANSFORM_PORTS = {
    "pass": (None, None),
    "parse-json": ("string", None),
    "field": ("object", None),
    "first": ("array", None),
    "count": ("array", "integer"),
    "format": (None, "string"),
}


def _node_contract(node, issues):
    if node.kind == "input":
        _signature(issues, node, {}, {"value": None})
    elif isinstance(node, DatasetNode):
        _signature(issues, node, {}, {"rows": "array"})
        rows = node.outputs.get("rows")
        if rows is not None and rows.items is None:
            issues.append(Issue(code="missing_row_schema", node_id=node.id, path="outputs.rows.items",
                                message="Dataset output must declare its row schema"))
    elif isinstance(node, TransformNode):
        if node.operation in ("order-clause", "order-price"):
            inputs = ({"message": "string", "index": "integer"} if node.operation == "order-clause"
                      else {"clause": "string", "fields": "object"})
            _signature(issues, node, inputs, {"value": "string" if node.operation == "order-clause" else "object"})
            if node.parameters:
                issues.append(Issue(code="invalid_parameters", node_id=node.id, path="parameters",
                                    message=f"'{node.operation}' takes no parameters"))
            return
        input_type, output_type = _TRANSFORM_PORTS[node.operation]
        _signature(issues, node, {"value": input_type}, {"value": output_type})
        field = node.parameters.get("field")
        if node.operation == "field":
            if node.parameters.keys() != {"field"} or not isinstance(field, str) or not field.strip():
                issues.append(Issue(code="invalid_parameters", node_id=node.id, path="parameters",
                                    message="Get object field requires one nonempty 'field' string"))
        elif node.parameters:
            issues.append(Issue(code="invalid_parameters", node_id=node.id, path="parameters",
                                message=f"'{node.operation}' takes no parameters"))
    elif node.kind == "output":
        _signature(issues, node, {"value": None}, {"value": None})
    elif isinstance(node, ToolNode):
        _signature(issues, node, {"value": None}, {"receipt": "object"})
    elif not node.inputs or not node.outputs:
        issues.append(Issue(code="invalid_ports", node_id=node.id,
                            message="Model and Reasoning nodes require named inputs and outputs"))
    elif isinstance(node, GenerationNode) and node.response == "text":
        if len(node.outputs) != 1 or next(iter(node.outputs.values())).type != "string":
            issues.append(Issue(code="invalid_ports", node_id=node.id, path="outputs",
                                message="A text response requires exactly one string output"))

    if node.operation in ("pass", "output") and "value" in node.inputs and "value" in node.outputs:
        if not _compatible(node.inputs["value"], node.outputs["value"]):
            issues.append(Issue(code="incompatible_ports", node_id=node.id, path="outputs.value",
                                message="A forwarded value must have a compatible output schema"))


def node_test_graph(node: BlueprintNode) -> Graph:
    issues = []
    _node_contract(node, issues)
    if issues:
        raise Problems(issues)
    return Graph({node.id: node}, [node.id], {node.id: {}})


def graph_for(blueprint: Blueprint) -> Graph:
    issues = []
    nodes = {}
    for node in blueprint.nodes:
        if node.id in nodes:
            issues.append(Issue(code="duplicate_node", node_id=node.id, message="Node ID must be unique"))
        nodes[node.id] = node
        _node_contract(node, issues)
        if isinstance(node, DatasetNode) and isinstance(node.binding, InlineRows) and "rows" in node.outputs:
            try:
                validate_value(node.binding.rows, node.outputs["rows"], node.id, "binding.rows")
            except Problems as exc:
                issues.extend(exc.issues)
    if not any(node.kind == "output" for node in nodes.values()):
        issues.append(Issue(code="missing_output", message="A blueprint needs at least one Output node"))

    sources: dict[str, dict[str, tuple[str, str]]] = {node: {} for node in nodes}
    children: dict[str, list[str]] = {node: [] for node in nodes}
    for index, edge in enumerate(blueprint.edges):
        src, dest = edge.source, edge.target
        source, target = nodes.get(src.node), nodes.get(dest.node)
        path = f"edges[{index}]"
        if source is None or target is None or src.port not in source.outputs or dest.port not in target.inputs:
            issues.append(Issue(code="unknown_port", node_id=dest.node, path=path,
                                message="Edge must connect a declared output to a declared input"))
            continue
        if dest.port in sources[dest.node]:
            issues.append(Issue(code="duplicate_connection", node_id=dest.node, path=path,
                                message="An input port can only have one source"))
            continue
        if not _compatible(source.outputs[src.port], target.inputs[dest.port]):
            issues.append(Issue(code="incompatible_ports", node_id=dest.node, path=path,
                                message=f"'{src.node}.{src.port}' is incompatible with '{dest.node}.{dest.port}'"))
        sources[dest.node][dest.port] = (src.node, src.port)
        children[src.node].append(dest.node)

    for node in nodes.values():
        for port in node.inputs.keys() - sources[node.id].keys():
            issues.append(Issue(code="unconnected_input", node_id=node.id, path=f"inputs.{port}",
                                message=f"Connect input port '{port}'"))
    pending = {node: len(ports) for node, ports in sources.items()}
    ready = deque(node for node, count in pending.items() if count == 0)
    order = []
    while ready:
        node_id = ready.popleft()
        order.append(node_id)
        for child in children[node_id]:
            pending[child] -= 1
            if pending[child] == 0:
                ready.append(child)
    if len(order) != len(nodes):
        issues.append(Issue(code="cycle", message="Blueprint connections must be acyclic"))
    if issues:
        raise Problems(issues)
    return Graph(nodes, order, sources)


def validate_request(graph: Graph, request: RunRequest) -> None:
    def check_keys(received, expected, path):
        issues = [
            Issue(code="missing_value", node_id=node, path=path, message=f"Provide {path} for '{node}'")
            for node in sorted(expected - received.keys())
        ]
        issues.extend(
            Issue(code="unexpected_value", node_id=node, path=path, message=f"Unexpected {path} for '{node}'")
            for node in sorted(received.keys() - expected)
        )
        return issues

    issues = check_keys(request.inputs, {n.id for n in graph.nodes.values() if n.kind == "input"}, "inputs")
    if request.mode == "fixture":
        fixture_nodes = {n.id for n in graph.nodes.values() if isinstance(n, (GenerationNode, ToolNode))}
    else:
        fixture_nodes = set()
    issues.extend(check_keys(request.fixtures, fixture_nodes, "fixtures"))
    if issues:
        raise Problems(issues)


_VALUE_TYPES = {
    "string": (str,), "integer": (int,), "number": (int, float), "boolean": (bool,),
    "object": (dict,), "array": (list,), "null": (type(None),),
}


def _shape_error(value, schema: ValueSchema, path: str) -> tuple[str, str] | None:
    if type(value) not in _VALUE_TYPES[schema.type]:
        return path, f"Expected {schema.type}"
    if schema.minimum is not None and value < schema.minimum:
        return path, f"Expected a number greater than or equal to {schema.minimum}"
    if schema.minLength is not None and len(value) < schema.minLength:
        return path, f"Expected at least {schema.minLength} characters"
    if schema.items is not None:
        for index, item in enumerate(value):
            if error := _shape_error(item, schema.items, f"{path}[{index}]"):
                return error
    if schema.type == "object":
        properties = schema.properties or {}
        for name in schema.required or ():
            if name not in value:
                return f"{path}.{name}", "Required property is missing"
        for name, item in value.items():
            if name in properties:
                if error := _shape_error(item, properties[name], f"{path}.{name}"):
                    return error
            elif schema.additionalProperties is False:
                return f"{path}.{name}", "Undeclared property"
    return None


def validate_value(value, schema: ValueSchema, node_id: str, path: str) -> None:
    if error := _shape_error(value, schema, path):
        raise Problems([Issue(code="invalid_value", node_id=node_id, path=error[0], message=error[1])])


def validate_ports(values, ports, node_id: str, path: str) -> None:
    missing = ports.keys() - values.keys()
    extra = values.keys() - ports.keys()
    if missing or extra:
        issues = [Issue(code="missing_value", node_id=node_id, path=f"{path}.{port}",
                        message="Required port value is missing") for port in sorted(missing)]
        issues.extend(Issue(code="unexpected_value", node_id=node_id, path=f"{path}.{port}",
                            message="Undeclared port value") for port in sorted(extra))
        raise Problems(issues)
    for name, schema in ports.items():
        validate_value(values[name], schema, node_id, f"{path}.{name}")
