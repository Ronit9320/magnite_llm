from __future__ import annotations

import threading
from contextlib import aclosing
from typing import AsyncIterator

from backend.inference.engines.base import BaseEngine
from backend.inference.engines.streaming import threaded_stream


class TorchPeftEngine(BaseEngine):
    """In-process transformers + PEFT inference for LoRA/QLoRA adapters.

    Used when an adapter is activated for the current inference session. The
    base model (fp16 on CUDA when available) is loaded with
    ``PeftModel.from_pretrained`` and chat/completion streams token-by-token
    through ``transformers.TextIteratorStreamer``.
    """

    name = "torch"

    def __init__(self, model_ref: str, adapter_path: str = ""):
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

        device = "cuda" if torch.cuda.is_available() else ("mps" if torch.backends.mps.is_available() else "cpu")
        dtype = torch.float16 if device != "cpu" else torch.float32

        tokenizer = AutoTokenizer.from_pretrained(self.model_ref)
        if tokenizer.pad_token is None:
            tokenizer.pad_token = tokenizer.eos_token

        model = AutoModelForCausalLM.from_pretrained(
            self.model_ref,
            torch_dtype=dtype,
            low_cpu_mem_usage=True,
        ).to(device)
        peft = PeftModel.from_pretrained(model, self.adapter_path) if self.adapter_path else model
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

    def _produce(self, prompt: str, kwargs: dict, stop):
        import torch
        from transformers import TextIteratorStreamer, StoppingCriteria, StoppingCriteriaList

        class Cancelled(StoppingCriteria):
            def __call__(self, input_ids, scores, **extra):
                return stop.is_set()

        streamer = TextIteratorStreamer(self._tokenizer, skip_prompt=True,
                                       skip_special_tokens=True, timeout=60.0)
        inputs = self._tokenizer(prompt, return_tensors="pt").to(self._model.device)
        temperature = float(kwargs.get("temperature", 0.7))
        gen_kwargs = dict(max_new_tokens=int(kwargs.get("max_tokens", 256)),
                          do_sample=temperature > 0, streamer=streamer,
                          stopping_criteria=StoppingCriteriaList([Cancelled()]))
        if temperature > 0:
            gen_kwargs.update(temperature=temperature, top_p=float(kwargs.get("top_p", 0.95)))
        errors = []

        def generate():
            try:
                with torch.inference_mode():
                    self._model.generate(**inputs, **gen_kwargs)
            except Exception as exc:
                errors.append(exc)
            finally:
                streamer.end()

        worker = threading.Thread(target=generate, daemon=True, name="torch-generate")
        worker.start()
        try:
            for chunk in streamer:
                if stop.is_set():
                    break
                if chunk:
                    yield chunk
        finally:
            stop.set()
            worker.join()
        if errors:
            raise errors[0]

    async def _stream(self, prompt: str, kwargs: dict) -> AsyncIterator[str]:
        async with aclosing(threaded_stream(lambda stop: self._produce(prompt, kwargs, stop))) as stream:
            async for text in stream:
                yield text

    async def chat_stream(self, messages, **kwargs) -> AsyncIterator[str]:
        if self._tokenizer is None:
            raise RuntimeError("Engine not loaded")
        prompt = self._messages_to_prompt(messages)
        async with aclosing(self._stream(prompt, kwargs)) as stream:
            async for delta in stream:
                yield delta

    async def complete_stream(self, prompt, **kwargs) -> AsyncIterator[str]:
        if self._tokenizer is None:
            raise RuntimeError("Engine not loaded")
        async with aclosing(self._stream(prompt, kwargs)) as stream:
            async for delta in stream:
                yield delta

    def count_tokens(self, text):
        return len(self._tokenizer.encode(text))

    def count_messages(self, messages):
        return self.count_tokens(self._messages_to_prompt(messages))

    def context_size(self):
        return int(getattr(self._model.config, "max_position_embeddings", 2048))

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
