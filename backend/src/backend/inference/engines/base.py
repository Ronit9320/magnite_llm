from __future__ import annotations

from abc import ABC, abstractmethod
from typing import AsyncIterator

from backend.inference.schemas import ChatMessage


class BaseEngine(ABC):
    """Interface every inference engine implements."""

    name: str = "base"

    def __init__(self, model_ref: str):
        self.model_ref = model_ref
        self._loaded = False

    @property
    def loaded(self) -> bool:
        return self._loaded

    @abstractmethod
    def load(self) -> None:
        """Bring the model into memory. Raises on failure."""

    @abstractmethod
    def chat_stream(self, messages: list[ChatMessage], **kwargs) -> AsyncIterator[str]:
        """Yield incremental text deltas for a chat-style conversation."""

    @abstractmethod
    def complete_stream(self, prompt: str, **kwargs) -> AsyncIterator[str]:
        """Yield incremental text deltas for a bare completion."""

    def unload(self) -> None:
        self._loaded = False

    def count_tokens(self, text: str) -> int:
        # Conservative fallback for remote engines without a local tokenizer.
        return len(text.encode("utf-8"))

    def context_size(self) -> int:
        return 2048

    def count_messages(self, messages) -> int:
        return 128 + sum(self.count_tokens(m.content) + 32 for m in messages)
