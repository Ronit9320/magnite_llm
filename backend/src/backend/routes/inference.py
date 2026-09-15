from __future__ import annotations

import json

from fastapi import APIRouter, HTTPException
from fastapi.responses import StreamingResponse
from pydantic import BaseModel

from backend.inference.schemas import ChatRequest, CompletionRequest, InferenceStatus
from backend.inference.supervisor import get_supervisor

router = APIRouter(prefix="/inference", tags=["inference"])


class AdapterActivateRequest(BaseModel):
    adapter_id: str = ""


def _sse(data: str) -> str:
    return f"data: {json.dumps(data)}\n\n"


@router.get("/status", response_model=InferenceStatus)
def status():
    return get_supervisor().status()


@router.post("/adapter", response_model=InferenceStatus)
def set_adapter(body: AdapterActivateRequest):
    """Activate (or clear) a trained adapter for the current inference session.

    Switching adapters restart the engine on the adapter's base model with the
    PEFT weights loaded. An empty ``adapter_id`` clears the active adapter.
    """
    sup = get_supervisor()
    adapter_id = (body.adapter_id or "").strip()
    try:
        if not adapter_id:
            sup.deactivate_adapter()
        else:
            sup.activate_adapter(adapter_id)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(status_code=500, detail=str(exc)) from exc
    return sup.status()


@router.post("/load")
def load_model(body: dict):
    model_ref = (body.get("model") or "").strip()
    engine = (body.get("engine") or "").strip() or None
    if not model_ref:
        raise HTTPException(status_code=400, detail="'model' is required")
    try:
        get_supervisor().load_model(model_ref, engine)
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(status_code=500, detail=str(exc)) from exc
    return get_supervisor().status()


@router.post("/unload")
def unload_model():
    get_supervisor().unload()
    return {"status": "unloaded"}


@router.post("/chat")
async def chat(req: ChatRequest):
    try:
        sup = get_supervisor()
        gen = sup.chat_stream(
            req.messages,
            max_tokens=req.max_tokens,
            temperature=req.temperature,
            top_p=req.top_p,
        )
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(status_code=500, detail=str(exc)) from exc

    async def event_stream():
        try:
            async for delta in gen:
                yield _sse({"delta": delta})
            yield _sse({"done": True})
        except Exception as exc:  # noqa: BLE001
            yield _sse({"error": str(exc)})

    return StreamingResponse(event_stream(), media_type="text/event-stream")


@router.post("/completion")
async def completion(req: CompletionRequest):
    try:
        sup = get_supervisor()
        gen = sup.complete_stream(
            req.prompt,
            max_tokens=req.max_tokens,
            temperature=req.temperature,
        )
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(status_code=500, detail=str(exc)) from exc

    async def event_stream():
        try:
            async for delta in gen:
                yield _sse({"delta": delta})
            yield _sse({"done": True})
        except Exception as exc:  # noqa: BLE001
            yield _sse({"error": str(exc)})

    return StreamingResponse(event_stream(), media_type="text/event-stream")
