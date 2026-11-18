from __future__ import annotations

import threading
from contextlib import aclosing, nullcontext
from typing import AsyncIterator

from backend.inference.engines.base import BaseEngine, EngineUnloaded
from backend.inference.engines.streaming import threaded_stream


def torch_device(torch) -> str:
    if torch.cuda.is_available():
        return "cuda"
    return "mps" if torch.backends.mps.is_available() else "cpu"


def messages_to_prompt(tokenizer, messages, thinking: bool | None = None) -> str:
    options = {} if thinking is None else {"enable_thinking": thinking}
    try:
        return tokenizer.apply_chat_template(messages, tokenize=False, add_generation_prompt=True, **options)
    except Exception:
        if thinking:
            raise
        lines = [f"{m['role']}: {m['content']}" for m in messages if m["content"].strip()]
        return "\n".join(lines) + "\nassistant:"


def tools_rendered(prompt: str, tools: list[dict]) -> bool:
    """A template that ignores ``tools`` has no native tool format."""
    return all(tool["function"]["name"] in prompt for tool in tools)


class TorchPeftEngine(BaseEngine):
    """In-process transformers + PEFT inference for LoRA/QLoRA adapters.

    Used when an adapter is activated for the current inference session. The
    base model (fp16 on CUDA when available) is loaded with
    ``PeftModel.from_pretrained`` and chat/completion streams token-by-token
    through ``transformers.TextIteratorStreamer``.
    """

    name = "torch"

    def __init__(self, model_ref: str, adapter_path: str = "", *, offline_only: bool = False):
        super().__init__(model_ref)
        self.adapter_path = adapter_path
        self._model = None
        self._tokenizer = None
        self.offline_only = offline_only
        self._blueprint_adapter: tuple[str, str] | None = None

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

        device = torch_device(torch)
        dtype = torch.float16 if device != "cpu" else torch.float32

        local = {"local_files_only": True, "trust_remote_code": False} if self.offline_only else {}
        tokenizer = AutoTokenizer.from_pretrained(self.model_ref, **local)
        if tokenizer.pad_token is None:
            tokenizer.pad_token = tokenizer.eos_token

        model = AutoModelForCausalLM.from_pretrained(
            self.model_ref,
            torch_dtype=dtype,
            low_cpu_mem_usage=True,
            **({"use_safetensors": True, **local} if self.offline_only else {}),
        ).to(device)
        peft = PeftModel.from_pretrained(model, self.adapter_path) if self.adapter_path else model
        peft.eval()

        self._model = peft
        self._tokenizer = tokenizer
        self._loaded = True

    def _messages_to_prompt(self, messages) -> str:
        raw = [{"role": m.role.value, "content": m.content} for m in messages]
        return messages_to_prompt(self._tokenizer, raw)

    def _select_blueprint_adapter(self, path: str) -> None:
        """Keep at most one selected adapter on a Blueprint-owned base model."""
        if self._blueprint_adapter is None:
            from peft import PeftModel

            self._model = PeftModel.from_pretrained(self._model, path, adapter_name="first",
                                                    local_files_only=True)
            self._blueprint_adapter = (path, "first")
        elif path != self._blueprint_adapter[0]:
            _, previous = self._blueprint_adapter
            name = "second" if previous == "first" else "first"
            self._model.load_adapter(path, adapter_name=name, local_files_only=True)
            self._model.set_adapter(name)
            self._model.delete_adapter(previous)
            self._blueprint_adapter = (path, name)
        self._model.eval()

    def generate_blueprint(self, messages, *, max_tokens: int, adapter: str,
                           reasoning: bool, stop) -> str:
        """Select an adapter and generate synchronously under the supervisor lock."""
        import torch
        from transformers import StoppingCriteria, StoppingCriteriaList

        if not self.offline_only or self._model is None:
            raise RuntimeError("Blueprint generation requires a loaded local engine")
        if adapter:
            self._select_blueprint_adapter(adapter)
        tokenizer = self._tokenizer
        prompt = messages_to_prompt(tokenizer, messages, thinking=reasoning)
        inputs = tokenizer(prompt, return_tensors="pt", add_special_tokens=False).to(self._model.device)
        prompt_length = inputs["input_ids"].shape[1]
        if prompt_length + max_tokens > self.context_size():
            raise ValueError(f"Prompt ({prompt_length} tokens) and output budget ({max_tokens} tokens) "
                             f"exceed the model context ({self.context_size()} tokens)")
        if stop.is_set():
            raise RuntimeError("Blueprint generation cancelled")

        class Cancelled(StoppingCriteria):
            def __call__(self, input_ids, scores, **kwargs):
                return stop.is_set()

        selected = self._model.disable_adapter() if not adapter and self._blueprint_adapter else nullcontext()
        with torch.inference_mode(), selected:
            result = self._model.generate(**inputs, max_new_tokens=max_tokens, do_sample=False,
                                          pad_token_id=tokenizer.pad_token_id,
                                          stopping_criteria=StoppingCriteriaList([Cancelled()]))
        tokens = result[0, prompt_length:].tolist()
        if reasoning:
            ending = tokenizer.encode("</think>", add_special_tokens=False)
            boundary = next((i + len(ending) for i in range(len(tokens))
                             if ending and tokens[i:i + len(ending)] == ending), None)
            if boundary is None:
                raise ValueError("The reasoning model did not finish its thinking within the token budget")
            tokens = tokens[boundary:]
        return tokenizer.decode(tokens, skip_special_tokens=True).strip()

    def _produce(self, prompt: str, kwargs: dict, stop):
        import torch
        from transformers import TextIteratorStreamer, StoppingCriteria, StoppingCriteriaList

        class Cancelled(StoppingCriteria):
            def __call__(self, input_ids, scores, **extra):
                return stop.is_set()

        streamer = TextIteratorStreamer(self._tokenizer, skip_prompt=True,
                                       skip_special_tokens=True, timeout=60.0)
        # A prompt rendered by the chat template already carries its special tokens.
        inputs = self._tokenizer(prompt, return_tensors="pt",
                                 add_special_tokens=not kwargs.get("templated", False)).to(self._model.device)
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
        if kwargs.pop("tool_mode", False):
            raw = [{"role": m.role.value, "content": m.content} for m in messages]
            prompt = messages_to_prompt(self._tokenizer, raw, thinking=False)
        else:
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
        tokenizer = self._tokenizer
        if tokenizer is None:
            raise EngineUnloaded("The model was unloaded")
        return len(tokenizer.encode(text))

    def count_messages(self, messages):
        return self.count_tokens(self._messages_to_prompt(messages))

    def render_tool_prompt(self, messages, tools):
        tokenizer = self._tokenizer
        if tokenizer is None:
            raise EngineUnloaded("The model was unloaded")
        try:
            prompt = tokenizer.apply_chat_template(messages, tools=tools, tokenize=False,
                                                   add_generation_prompt=True, enable_thinking=False)
        except Exception:
            return None
        return prompt if tools_rendered(prompt, tools) else None

    def context_size(self):
        model = self._model
        if model is None:
            raise EngineUnloaded("The model was unloaded")
        return int(getattr(model.config, "max_position_embeddings", 2048))

    def unload(self) -> None:
        super().unload()
        self._model = None
        self._tokenizer = None
        self._blueprint_adapter = None
        try:
            import torch

            if torch.cuda.is_available():
                torch.cuda.empty_cache()
            elif self.offline_only and torch.backends.mps.is_available():
                torch.mps.empty_cache()
        except Exception:  # noqa: BLE001
            pass
