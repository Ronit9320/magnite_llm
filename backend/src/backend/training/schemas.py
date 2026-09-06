from __future__ import annotations

from typing import Optional

from pydantic import BaseModel, Field


class DatasetRecord(BaseModel):
    id: str
    name: str
    format: str = Field(description="jsonl|csv|text")
    path: str = Field(description="Normalized JSONL file on disk")
    rows: int = 0
    size_kb: float = 0.0
    source: str = Field(default="", description="Original local path or 'upload'")
    created_at: str = ""


class TrainingConfig(BaseModel):
    """Hyperparameters and inputs for a LoRA/QLoRA training job."""

    base_model: str = Field(description="Registered model id, HF repo id, or local dir")
    dataset_id: str = Field(description="Must reference an ingested dataset")
    name: str = Field(default="", description="Adapter name; auto-generated when empty")

    rank: int = Field(8, ge=1, le=512)
    alpha: int = Field(16, ge=1, le=1024)
    target_modules: str = Field("q_proj,v_proj", description="Comma-separated or 'all-linear'")
    quantization: str = Field("none", description="none|4bit|8bit")

    epochs: int = Field(3, ge=1, le=100)
    max_steps: int = Field(0, ge=0, description="0 = derive from epochs")
    learning_rate: float = Field(2e-4, gt=0.0, le=1.0)
    batch_size: int = Field(1, ge=1, le=128)
    warmup_ratio: float = Field(0.03, ge=0.0, le=1.0)
    max_length: int = Field(512, ge=8, le=8192)
    seed: int = Field(42)


class TrainingJob(BaseModel):
    id: str
    name: str
    status: str = Field(description="queued|running|done|error|cancelled")
    base_model: str
    dataset_id: str
    config: dict = Field(default_factory=dict)
    progress: float = 0.0
    current_step: int = 0
    total_steps: int = 0
    loss: Optional[float] = None
    history: list[dict] = Field(default_factory=list)
    message: str = ""
    error: Optional[str] = None
    adapter_id: Optional[str] = None
    created_at: str = ""
    started_at: Optional[str] = None
    finished_at: Optional[str] = None


class DatasetIngestRequest(BaseModel):
    name: str = Field(default="", description="Display name; default from filename")
    path: str = Field(default="", description="Local file path to ingest")
    content: str = Field(default="", description="Raw content (requires 'name')")
    format: str = Field(default="", description="jsonl|csv|text; auto by extension")