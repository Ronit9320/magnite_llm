from __future__ import annotations

from enum import Enum
from typing import Optional

from pydantic import BaseModel, Field


class ChatRole(str, Enum):
    system = "system"
    user = "user"
    assistant = "assistant"


class ChatMessage(BaseModel):
    role: ChatRole
    content: str


class RagChatOptions(BaseModel):
    """When enabled, retrieved document context is prepended to the chat."""

    enabled: bool = False
    top_k: Optional[int] = Field(default=None, ge=1, le=50)
    min_score: Optional[float] = Field(default=None, ge=0.0, le=1.0)


class ChatRequest(BaseModel):
    messages: list[ChatMessage]
    model: Optional[str] = None
    max_tokens: int = Field(default=256, ge=1, le=8192)
    temperature: float = Field(default=0.7, ge=0.0, le=2.0)
    top_p: float = Field(default=0.95, ge=0.0, le=1.0)
    stream: bool = True
    rag: Optional[RagChatOptions] = None


class CompletionRequest(BaseModel):
    prompt: str
    model: Optional[str] = None
    max_tokens: int = Field(default=256, ge=1, le=8192)
    temperature: float = Field(default=0.7, ge=0.0, le=2.0)
    stream: bool = True


class EngineInfo(BaseModel):
    name: str = Field(description="'llamacpp' or 'vllm'")
    available: bool
    selected: bool
    description: str
    detail: str = ""


class InferenceStatus(BaseModel):
    engine: Optional[EngineInfo]
    loaded_model: Optional[str] = None
    model_source: str = ""
    backend_available: bool
    active_adapter: Optional[str] = None
    active_adapter_id: Optional[str] = None
