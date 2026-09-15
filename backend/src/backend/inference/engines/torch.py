from __future__ import annotations

import asyncio
import threading
from typing import AsyncIterator

from backend.inference.engines.base import BaseEngine

_DONE = None


class TorchPeftEngine(BaseEngine):
    """In-process transformers + PEFT inference for LoRA/QLoRA adapters.

    Used when an adapter is activated for the current inference session. The
    base model (fp16 on CUDA when available) is loaded with
    ``PeftModel.from_pretrained`` and chat/completion streams token-by-token
    through ``transformers.TextIteratorStreamer``.
    """

    name = "torch"

    def __init__(self, model_ref: str, adapter_path: str):
        super().__init__(model_ref)
        self.adapter_path = adapter_path
        self._model = None
        self._tokenizer = None

    @staticmethod
    def is_available() -> bool:
        try:
            import torch  # noqa: F401
            import transformers  # noqa: F401
            import peft  # noqa: F401
            return True
        except ImportError:
            return False

    def load(self) -> None:
        import torch
        from peft import PeftModel
        from transformers import AutoModelForCausalLM, AutoTokenizer

        dtype = torch.float16 if torch.cuda.is_available() else torch.float32
        device = "cuda" if torch.cuda.is_available() else "cpu"

        tokenizer = AutoTokenizer.from_pretrained(self.model_ref)
        if tokenizer.pad_token is None:
            tokenizer.pad_token = tokenizer.eos_token

        model = AutoModelForCausalLM.from_pretrained(
            self.model_ref,
            torch_dtype=dtype,
            low_cpu_mem_usage=True,
        ).to(device)
        peft = PeftModel.from_pretrained(model, self.adapter_path)
        peft.eval()

        self._model = peft
        self._tokenizer = tokenizer
        self._loaded = True

    def _messages_to_prompt(self, messages) -> str:
        raw = [{"role": m.role.value, "content": m.content} for m in messages]
        try:
            return self._tokenizer.apply_chat_template(
                raw, tokenize=False, add_generation_prompt=True
            )
        except Exception:  # noqa: BLE001 -- no chat template (e.g. GPT-2)
            lines = [f"{m['role']}: {m['content']}" for m in raw if m["content"].strip()]
            return "\n".join(lines) + "\nassistant:"

    def _pump(self, prompt: str, kwargs: dict, queue: asyncio.Queue) -> None:
        import torch
        from transformers import TextIteratorStreamer

        try:
            streamer = TextIteratorStreamer(
                self._tokenizer,
                skip_prompt=True,
                skip_special_tokens=True,
                timeout=60.0,
            )
            inputs = self._tokenizer(prompt, return_tensors="pt").to(
                "cuda" if torch.cuda.is_available() else "cpu"
            )
            temperature = float(kwargs.get("temperature", 0.7))
            gen_kwargs = {
                "max_new_tokens": int(kwargs.get("max_tokens", 256)),
                "do_sample": temperature > 0.0,
                "streamer": streamer,
            }
            if gen_kwargs["do_sample"]:
                gen_kwargs["temperature"] = temperature
                gen_kwargs["top_p"] = float(kwargs.get("top_p", 0.95))
            with torch.inference_mode():
                self._model.generate(**inputs, **gen_kwargs)
            for chunk in streamer:
                if chunk:
                    queue.put_nowait(chunk)
            queue.put_nowait(_DONE)
        except Exception as exc:  # noqa: BLE001
            queue.put_nowait(exc)

    async def _stream(self, prompt: str, kwargs: dict) -> AsyncIterator[str]:
        queue: asyncio.Queue = asyncio.Queue()
        thread = threading.Thread(
            target=self._pump, args=(prompt, kwargs, queue), daemon=True
        )
        thread.start()
        while True:
            item = await queue.get()
            if isinstance(item, Exception):
                raise item
            if item is _DONE:
                break
            if item:
                yield item

    async def chat_stream(self, messages, **kwargs) -> AsyncIterator[str]:
        if self._tokenizer is None:
            raise RuntimeError("Engine not loaded")
        prompt = self._messages_to_prompt(messages)
        async for delta in self._stream(prompt, kwargs):
            yield delta

    async def complete_stream(self, prompt, **kwargs) -> AsyncIterator[str]:
        if self._tokenizer is None:
            raise RuntimeError("Engine not loaded")
        async for delta in self._stream(prompt, kwargs):
            yield delta

    def unload(self) -> None:
        super().unload()
        self._model = None
        self._tokenizer = None
        try:
            import torch

            torched = torch
            if torched.cuda.is_available():
                torched.cuda.empty_cache()
        except Exception:  # noqa: BLE001
            pass