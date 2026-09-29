from __future__ import annotations

import os
import threading
from pathlib import Path
from contextlib import aclosing
from typing import AsyncIterator

from backend.inference.engines.base import BaseEngine
from backend.inference.engines.streaming import threaded_stream

_GUARD = threading.Lock()

# HF repos known to host a GGUF we can pull by convention.
# Fallback filename patterns used if repo listing is unavailable.
_FALLBACK_FILENAMES = (
    "model.gguf",
    "*.gguf",
)


class LlamaCppEngine(BaseEngine):
    """In-process llama.cpp engine via llama-cpp-python. Requires a GGUF file."""

    name = "llamacpp"

    def __init__(self, model_ref: str, n_ctx: int = 2048, n_threads: int = 0,
                 n_gpu_layers: int = 0):
        super().__init__(model_ref)
        self.n_ctx = n_ctx
        self.n_threads = n_threads
        self.n_gpu_layers = n_gpu_layers
        self._llm = None

    @staticmethod
    def is_available() -> bool:
        try:
            import llama_cpp  # noqa: F401
            return True
        except ImportError:
            return False

    def _resolve_gguf_path(self) -> str:
        """If model_ref is a local path, use it. Otherwise pull a GGUF from HF."""
        if os.path.isfile(self.model_ref):
            return self.model_ref

        if os.path.isdir(self.model_ref):
            files = sorted(Path(self.model_ref).rglob("*.gguf"), key=lambda p: p.stat().st_size)
            if not files:
                raise ValueError("This model has no GGUF weights. Select the torch engine or a GGUF model.")
            return str(files[0])

        # model_ref is an HF repo id; download a gguf file from it.
        from huggingface_hub import hf_hub_download, list_repo_files

        files = list(list_repo_files(self.model_ref))
        ggufs = [f for f in files if f.lower().endswith(".gguf")]
        if not ggufs:
            raise RuntimeError(
                f"No .gguf file found in '{self.model_ref}'. "
                "Convert the model or point to a GGUF repo.")

        # Prefer a stable default name, else the smallest file.
        chosen = next((f for f in ggufs if f == "model.gguf"), None)
        if chosen is None:
            chosen = min(ggufs, key=lambda f: len(f))
            if not chosen.lower().endswith(".gguf"):
                chosen = ggufs[0]
        return hf_hub_download(self.model_ref, chosen)

    def load(self) -> None:
        from llama_cpp import Llama

        path = self._resolve_gguf_path()
        # Building/binding the model is CPU-bound and can block the event loop.
        with _GUARD:
            self._llm = Llama(
                model_path=path,
                n_ctx=self.n_ctx,
                n_threads=self.n_threads or None,
                n_gpu_layers=self.n_gpu_layers,
                verbose=False,
            )
        self._loaded = True

    def _ensure(self):
        if self._llm is None:
            self.load()
        return self._llm

    def _chat(self, messages, stop, **kwargs) -> AsyncIterator[str]:
        llm = self._ensure()
        api_messages = [{"role": m.role.value, "content": m.content} for m in messages]
        stream = llm.create_chat_completion(
            messages=api_messages,
            max_tokens=kwargs.get("max_tokens", 256),
            temperature=kwargs.get("temperature", 0.7),
            top_p=kwargs.get("top_p", 0.95),
            stream=True,
        )
        for chunk in stream:
            if stop.is_set():
                break
            choices = chunk.get("choices")
            if not choices:
                continue
            delta = choices[0].get("delta") or {}
            text = delta.get("content")
            if text:
                yield text

    def _complete(self, prompt, stop, **kwargs) -> AsyncIterator[str]:
        llm = self._ensure()
        stream = llm.create_completion(
            prompt=prompt,
            max_tokens=kwargs.get("max_tokens", 256),
            temperature=kwargs.get("temperature", 0.7),
            top_p=kwargs.get("top_p", 0.95),
            stream=True,
        )
        for chunk in stream:
            if stop.is_set():
                break
            choices = chunk.get("choices")
            if not choices:
                continue
            text = choices[0].get("text")
            if text:
                yield text

    async def chat_stream(self, messages, **kwargs):
        async with aclosing(threaded_stream(lambda stop: self._chat(messages, stop, **kwargs))) as stream:
            async for text in stream:
                yield text

    async def complete_stream(self, prompt, **kwargs):
        async with aclosing(threaded_stream(lambda stop: self._complete(prompt, stop, **kwargs))) as stream:
            async for text in stream:
                yield text

    def count_tokens(self, text):
        return len(self._llm.tokenize(text.encode("utf-8")))

    def context_size(self):
        return self.n_ctx

    def unload(self) -> None:
        super().unload()
        self._llm = None
