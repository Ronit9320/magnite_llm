from __future__ import annotations

import json
import time
import uuid
import asyncio
import anyio
from contextlib import aclosing, suppress
from typing import Literal

from fastapi import APIRouter, HTTPException, Request
from fastapi.responses import StreamingResponse, Response
from pydantic import BaseModel, Field

from backend.inference.schemas import ChatMessage, ChatRequest, ChatRole, CompletionRequest, InferenceStatus
from backend.inference.supervisor import get_supervisor
from backend.rag.context import injectable_context
from backend.request_work import request_work, watch_disconnect, ClientDisconnected

router = APIRouter(prefix="/inference", tags=["inference"])


class AdapterActivateRequest(BaseModel):
    adapter_id: str = ""


def _sse(data: str) -> str:
    return f"data: {json.dumps(data)}\n\n"


def _with_rag_context(req: ChatRequest, engine) -> list[ChatMessage]:
    messages = list(req.messages)
    limit = engine.context_size() - req.max_tokens
    if engine.count_messages(messages) > limit:
        raise ValueError("Conversation plus requested output exceeds the model context. Start a new chat or reduce max tokens.")
    if not (req.rag and req.rag.enabled):
        return messages
    last_user = next((m for m in reversed(messages) if m.role == ChatRole.user), None)
    if last_user is None:
        raise ValueError("RAG requires a user message")
    context = injectable_context(last_user.content, top_k=req.rag.top_k, min_score=req.rag.min_score)
    low, high = 0, len(context)
    while low < high:
        mid = (low + high + 1) // 2
        candidate = [ChatMessage(role=ChatRole.system, content=context[:mid])] + messages
        if engine.count_messages(candidate) <= limit:
            low = mid
        else:
            high = mid - 1
    if low < min(128, len(context)):
        raise ValueError("No room for retrieved context. Start a new chat or reduce max tokens.")
    return [ChatMessage(role=ChatRole.system, content=context[:low])] + messages


@router.get("/status", response_model=InferenceStatus)
def status():
    return get_supervisor().status()


@router.post("/adapter", response_model=InferenceStatus)
async def set_adapter(body: AdapterActivateRequest, request: Request):
    """Activate (or clear) a trained adapter for the current inference session.

    Switching adapters restart the engine on the adapter's base model with the
    PEFT weights loaded. An empty ``adapter_id`` clears the active adapter.
    """
    sup = get_supervisor()
    adapter_id = (body.adapter_id or "").strip()
    try:
        if not adapter_id:
            await request_work(request, lambda cancelled: sup.deactivate_adapter())
        else:
            await request_work(request, lambda cancelled: sup.activate_adapter(adapter_id, cancelled))
    except ClientDisconnected:
        return Response(status_code=499)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except Exception as exc:  # noqa: BLE001
        raise HTTPException(status_code=500, detail=str(exc)) from exc
    return sup.status()


class LoadRequest(BaseModel):
    model: str = Field(min_length=1)
    engine: Literal["llamacpp", "vllm", "torch", "ollama"] | None = None


class RuntimeSettings(BaseModel):
    engine_override: int = Field(0, ge=0, le=2)


@router.post("/settings")
def runtime_settings(body: RuntimeSettings):
    get_supervisor().engine_override = body.engine_override
    return {"engine_override": body.engine_override}


@router.post("/load")
async def load_model(body: LoadRequest, request: Request):
    try:
        await request_work(request, lambda cancelled:
                           get_supervisor().load_model(body.model.strip(), body.engine, cancelled))
    except ClientDisconnected:
        return Response(status_code=499)
    except ValueError as exc:
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except RuntimeError as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from exc
    except Exception as exc:
        raise HTTPException(status_code=500, detail=str(exc)) from exc
    return get_supervisor().status()


@router.post("/unload")
def unload_model():
    try:
        get_supervisor().unload()
    except RuntimeError as exc:
        raise HTTPException(status_code=409, detail=str(exc)) from exc
    return {"status": "unloaded"}


class InferenceStreamResponse(StreamingResponse):
    """Close workers even when ASGI send fails between generator iterations."""

    def __init__(self, events, source):
        super().__init__(events, media_type="text/event-stream")
        self.source = source

    async def __call__(self, scope, receive, send):
        try:
            await super().__call__(scope, receive, send)
        finally:
            with anyio.CancelScope(shield=True):
                await self.body_iterator.aclose()
                await self.source.aclose()


async def _first_token(gen, request):
    stop_watching = asyncio.Event()
    token = asyncio.create_task(anext(gen, None))
    disconnect = asyncio.create_task(watch_disconnect(request, stop_watching))
    try:
        done, _ = await asyncio.wait((token, disconnect), return_when=asyncio.FIRST_COMPLETED)
        if token in done:
            return await token
        raise asyncio.CancelledError("Client disconnected before generation started")
    finally:
        # Request.is_disconnected uses an AnyIO cancellation scope. A task.cancel
        # racing that scope can be swallowed, so stop this watcher explicitly.
        stop_watching.set()
        if not token.done():
            token.cancel()
        with anyio.CancelScope(shield=True):
            with suppress(asyncio.CancelledError):
                await disconnect
            with suppress(asyncio.CancelledError, Exception):
                await token


async def _response(gen, stream, model, chat_response=True, request=None):
    # Advance once before sending HTTP headers so startup errors get a proper status.
    try:
        first = await _first_token(gen, request) if request is not None else await anext(gen, None)
    except asyncio.CancelledError:
        with anyio.CancelScope(shield=True):
            await gen.aclose()
        return Response(status_code=499)
    except (ValueError, RuntimeError) as exc:
        await gen.aclose()
        raise HTTPException(status_code=400, detail=str(exc)) from exc
    except Exception as exc:
        await gen.aclose()
        raise HTTPException(status_code=500, detail=str(exc)) from exc

    if not stream:
        try:
            async with aclosing(gen):
                parts = [first or ""]
                while True:
                    delta = (await _first_token(gen, request) if request is not None
                             else await anext(gen, None))
                    if delta is None:
                        break
                    parts.append(delta)
                text = "".join(parts)
        except asyncio.CancelledError:
            with anyio.CancelScope(shield=True):
                await gen.aclose()
            return Response(status_code=499)
        except Exception as exc:
            raise HTTPException(status_code=500, detail=str(exc)) from exc
        choice = {"index": 0, "finish_reason": "stop"}
        choice.update({"message": {"role": "assistant", "content": text}} if chat_response else {"text": text})
        return {"id": "magnite-" + uuid.uuid4().hex, "created": int(time.time()),
                "model": model, "object": "chat.completion" if chat_response else "text_completion",
                "choices": [choice]}

    async def events():
        async with aclosing(gen):
            try:
                if first:
                    yield _sse({"delta": first})
                async for delta in gen:
                    yield _sse({"delta": delta})
                yield _sse({"done": True})
            except Exception as exc:
                yield _sse({"error": str(exc)})
    return InferenceStreamResponse(events(), gen)


def _validate_model(requested, sup):
    if not requested or requested == sup.model_ref:
        return
    from backend.models.resolution import resolve_base, registered_model
    resolved = (registered_model(requested).ref if requested.startswith("ollama:")
                else resolve_base(requested))
    if resolved != sup.model_ref:
        raise ValueError(f"Requested model '{requested}' is not the loaded model '{sup.model_ref}'")


def _prepare_chat(req, sup, engine):
    _validate_model(req.model, sup)
    return _with_rag_context(req, engine)


@router.post("/chat")
async def chat(req: ChatRequest, request: Request):
    sup = get_supervisor()
    gen = sup.chat_stream(req.messages, max_tokens=req.max_tokens,
                          temperature=req.temperature, top_p=req.top_p,
                          prepare=lambda engine: _prepare_chat(req, sup, engine))
    return await _response(gen, req.stream, sup.model_ref, request=request)


@router.post("/completion")
async def completion(req: CompletionRequest, request: Request):
    sup = get_supervisor()
    def prepare(engine):
        _validate_model(req.model, sup)
        if engine.count_tokens(req.prompt) + req.max_tokens > engine.context_size():
            raise ValueError("Prompt and requested output exceed the model context window")
        return req.prompt
    gen = sup.complete_stream(req.prompt, max_tokens=req.max_tokens,
                              temperature=req.temperature, prepare=prepare)
    return await _response(gen, req.stream, sup.model_ref, chat_response=False, request=request)
