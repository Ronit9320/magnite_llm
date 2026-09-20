from __future__ import annotations

from typing import Optional

from pydantic import BaseModel, Field


class RagConfig(BaseModel):
    """Retrieval configuration (chunking + query defaults)."""

    chunk_size: int = Field(512, ge=64, le=4096, description="Target chunk size in tokens")
    chunk_overlap: int = Field(64, ge=0, le=1024, description="Overlap between chunks (tokens)")
    top_k: int = Field(4, ge=1, le=20, description="Default chunks retrieved per query")
    min_score: float = Field(0.0, ge=0.0, le=1.0, description="Similarity threshold (0 = off)")
    model: str = Field("all-MiniLM-L6-v2", description="sentence-transformers model id")


class RagDocument(BaseModel):
    id: str
    name: str
    kind: str = Field(description="txt|md|html|pdf")
    path: str = Field(default="", description="Source path ('' for raw content)")
    chunks: int = 0
    tokens: int = 0
    size_kb: float = 0.0
    created_at: str = ""


class RagIngestRequest(BaseModel):
    path: str = Field(default="", description="Local file path to ingest")
    content: str = Field(default="", description="Raw content (requires 'name')")
    kind: str = Field(default="", description="txt|md|html|pdf; detected from name/path when empty")
    name: str = Field(default="", description="Display name; default from filename")
    chunk_size: Optional[int] = None
    chunk_overlap: Optional[int] = None


class RagQueryRequest(BaseModel):
    query: str
    top_k: Optional[int] = Field(default=None, ge=1, le=50)
    min_score: Optional[float] = Field(default=None, ge=0.0, le=1.0)


class RagChunk(BaseModel):
    document_id: str
    document_name: str
    index: int
    text: str
    tokens: int
    score: float


class RagQueryResponse(BaseModel):
    query: str
    results: list[RagChunk]
    engine_ready: bool = True