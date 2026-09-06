from __future__ import annotations

from typing import Optional

from pydantic import BaseModel, Field


class AdapterRecord(BaseModel):
    """A trained LoRA/QLoRA adapter registered in the store."""

    id: str
    name: str
    base_model: str = Field(description="Base model ref/id used for training")
    architecture: str = ""
    rank: int = 0
    alpha: int = 0
    target_modules: str = ""
    method: str = "lora"  # lora | qlora
    quantization: str = "none"  # none|4bit|8bit
    path: str = Field(description="Directory holding the PEFT adapter files")
    size_mb: float = 0.0
    training_job_id: str = ""
    trained_at: str = ""
    meta: dict = Field(default_factory=dict)
    status: str = "ready"