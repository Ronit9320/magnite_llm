from __future__ import annotations

from typing import Optional

from pydantic import BaseModel, Field


class ModelRecord(BaseModel):
    """A model registered in the local registry."""

    id: str
    name: str
    source: str = Field(description="'huggingface' or 'ollama'")
    ref: str = Field(description="Original repo id / ollama name")
    path: str = Field(description="Local cache path (HF dir) or ollama name")
    architecture: str = ""
    size_mb: float = 0.0
    license: str = ""
    meta: dict = Field(default_factory=dict)
    downloaded_at: str = ""
    status: str = "ready"


class HubSearchResult(BaseModel):
    id: str
    repo_id: str
    downloads: int = 0
    likes: int = 0
    gated: bool = False
    license: str = ""
    tags: list[str] = Field(default_factory=list)
    source: str = "huggingface"


class PullRequest(BaseModel):
    ref: str = Field(description="HF repo id or Ollama model name")
    source: str = Field("huggingface", description="'huggingface' or 'ollama'")


class PullJob(BaseModel):
    id: str
    ref: str
    source: str
    status: str = Field(description="queued|running|done|error")
    progress: float = 0.0
    message: str = ""
    error: Optional[str] = None


class HealthResponse(BaseModel):
    status: str
    version: str


def model_id(source: str, ref: str) -> str:
    """URL-safe registry id. Slashes are encoded as '--' (HF cache convention)
    so ids can appear unencoded in REST paths."""
    return f"{source}:{ref.replace('/', '--')}"