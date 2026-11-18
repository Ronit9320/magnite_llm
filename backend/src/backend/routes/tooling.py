"""Authenticated tool chat and shared run approvals."""

from __future__ import annotations

from pathlib import PurePath
import re
from typing import Annotated
from uuid import uuid4

from fastapi import APIRouter, Depends, HTTPException
from pydantic import Field

from backend.blueprints.runner import RunnerUnavailable
from backend.blueprints.schemas import Blueprint, Contract, Problems, Revision, RunRequest, RunSummary, Text
from backend.blueprints.store import now
from backend.inference.schemas import ChatMessage
from backend.routes.blueprints import _BlueprintRoute, _invalid, blueprint_runner, require_tool_auth
from backend.tooling import pi_bridge
from backend.tooling.approvals import Approval, ApprovalConflict, Decision


router = APIRouter(tags=["tools"], route_class=_BlueprintRoute, dependencies=[Depends(require_tool_auth)])


class ToolChatRequest(Contract):
    messages: list[ChatMessage] = Field(min_length=1, max_length=64)
    working_directory: Annotated[Text, Field(min_length=1, max_length=4096)] | None = None


_CHAT_GRAPH = Blueprint.model_validate({
    "version": 1, "name": "Tool chat",
    "nodes": [
        {"id": "messages", "kind": "input", "operation": "input", "outputs": {"value": {"type": "array"}}},
        {"id": "assistant", "kind": "tool", "operation": "agent",
         "inputs": {"value": {"type": "array"}}, "outputs": {"receipt": {"type": "object"}}},
        {"id": "reply", "kind": "output", "operation": "output",
         "inputs": {"value": {"type": "object"}}, "outputs": {"value": {"type": "object"}}},
    ],
    "edges": [
        {"source": {"node": "messages", "port": "value"}, "target": {"node": "assistant", "port": "value"}},
        {"source": {"node": "assistant", "port": "receipt"}, "target": {"node": "reply", "port": "value"}},
    ],
})


@router.get("/inference/tool-runs/capabilities")
def tool_chat_capabilities():
    try:
        engine = pi_bridge.loaded_engine()
    except (OSError, ValueError, RuntimeError) as exc:
        return {"available": False, "detail": str(exc)}
    return {"available": True, "harness": "pi", "engine": engine.name, "model": engine.model_ref,
            "label": model_label(engine.model_ref), "native_tools": pi_bridge.native_tools(engine)}


def model_label(reference: str) -> str:
    """A short name for a repo ID, Hugging Face cache snapshot, or local file."""
    cached = re.search(r"models--[^/\\]*?--([^/\\]+)[/\\]snapshots", reference)
    if cached:
        return cached.group(1)
    return PurePath(reference).stem if reference.lower().endswith(".gguf") else PurePath(reference).name or reference


@router.post("/inference/tool-runs", response_model=RunSummary, status_code=202)
def start_tool_chat(body: ToolChatRequest):
    messages = [message.model_dump(mode="json") for message in body.messages]
    try:
        pi_bridge.chat_messages(messages)
        request = RunRequest(mode="local", inputs={"messages": messages}, working_directory=body.working_directory)
        revision = Revision(id=uuid4().hex, revision=1, updated_at=now(), blueprint=_CHAT_GRAPH)
        return blueprint_runner().start(revision, request, surface="chat", tool_chat=True)
    except Problems as exc:
        raise _invalid(exc) from exc
    except ValueError as exc:
        raise HTTPException(422, detail="Invalid tool chat messages") from exc
    except RunnerUnavailable as exc:
        raise HTTPException(503, detail=str(exc)) from exc


@router.get("/blueprints/runs/{run_id}/approvals", response_model=list[Approval], response_model_exclude_none=True)
def list_approvals(run_id: str):
    runner = blueprint_runner()
    if runner.store.summary(run_id) is None:
        raise HTTPException(404, detail="Run not found")
    return runner.broker.list(run_id)


@router.post("/blueprints/runs/{run_id}/approvals/{approval_id}/decision",
             response_model=Approval, response_model_exclude_none=True)
def decide_approval(run_id: str, approval_id: str, body: Decision):
    try:
        approval = blueprint_runner().broker.decide(run_id, approval_id, body)
    except ApprovalConflict as exc:
        raise HTTPException(409, detail=str(exc)) from exc
    if approval is None:
        raise HTTPException(404, detail="Approval not found for this run")
    return approval
