from __future__ import annotations

from typing import Optional

from pydantic import BaseModel, Field, model_validator


class RagConfig(BaseModel):
    """Retrieval configuration (chunking + query defaults)."""

    chunk_size: int = Field(320, ge=64, le=4096, description="Target chunk size in words (clamped to the embedding model's limit at ingest)")
    chunk_overlap: int = Field(40, ge=0, le=1024, description="Overlap between chunks in words")
    top_k: int = Field(4, ge=1, le=20, description="Default chunks retrieved per query")
    min_score: float = Field(0.0, ge=0.0, le=1.0, description="Similarity threshold (0 = off)")
    model: str = Field("all-MiniLM-L6-v2", description="sentence-transformers model id")
    truncate_dim: int = Field(0, ge=0, le=4096, description="Cut embeddings to this many dimensions (0 = full; needs a Matryoshka model)")

    @model_validator(mode="after")
    def valid_overlap(self):
        if self.chunk_overlap >= self.chunk_size:
            raise ValueError("Chunk overlap must be smaller than chunk size")
        if not self.model.strip():
            raise ValueError("Embedding model is required")
        return self


class RagConfigUpdate(BaseModel):
    chunk_size: Optional[int] = Field(default=None, ge=64, le=4096, description="Chunk size in words")
    chunk_overlap: Optional[int] = Field(default=None, ge=0, le=1024, description="Chunk overlap in words")
    top_k: Optional[int] = Field(default=None, ge=1, le=20)
    min_score: Optional[float] = Field(default=None, ge=0, le=1)
    model: Optional[str] = None
    truncate_dim: Optional[int] = Field(default=None, ge=0, le=4096)


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
    chunk_size: Optional[int] = Field(default=None, ge=64, le=4096, description="Chunk size in words")
    chunk_overlap: Optional[int] = Field(default=None, ge=0, le=1024, description="Chunk overlap in words")


class RagQueryRequest(BaseModel):
    query: str
    top_k: Optional[int] = Field(default=None, ge=1, le=50)
    min_score: Optional[float] = Field(default=None, ge=0.0, le=1.0)
    document_ids: Optional[list[str]] = None


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
