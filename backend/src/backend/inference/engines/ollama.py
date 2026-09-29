import json
import httpx

from backend.inference.engines.base import BaseEngine
from backend.models.registry import OLLAMA_HOST


class OllamaEngine(BaseEngine):
    name = "ollama"

    def load(self):
        response = httpx.post(f"{OLLAMA_HOST}/api/show", json={"model": self.model_ref}, timeout=30)
        response.raise_for_status()
        self._loaded = True

    async def _stream(self, route, payload):
        async with httpx.AsyncClient(timeout=httpx.Timeout(120, connect=10)) as client:
            async with client.stream("POST", f"{OLLAMA_HOST}/api/{route}", json=payload) as response:
                response.raise_for_status()
                async for line in response.aiter_lines():
                    if not line:
                        continue
                    event = json.loads(line)
                    if event.get("error"):
                        raise RuntimeError(event["error"])
                    text = event.get("message", {}).get("content", "") if route == "chat" else event.get("response", "")
                    if text:
                        yield text

    async def chat_stream(self, messages, **kwargs):
        from contextlib import aclosing
        payload = self._payload(kwargs)
        payload["messages"] = [m.model_dump() for m in messages]
        async with aclosing(self._stream("chat", payload)) as stream:
            async for text in stream:
                yield text

    async def complete_stream(self, prompt, **kwargs):
        from contextlib import aclosing
        payload = self._payload(kwargs)
        payload["prompt"] = prompt
        async with aclosing(self._stream("generate", payload)) as stream:
            async for text in stream:
                yield text

    def _payload(self, kwargs):
        return {"model": self.model_ref, "stream": True,
                "options": {"num_predict": kwargs.get("max_tokens", 256),
                            "temperature": kwargs.get("temperature", .7),
                            "top_p": kwargs.get("top_p", .95)}}

    def unload(self):
        if self.loaded:
            response = httpx.post(f"{OLLAMA_HOST}/api/generate",
                                 json={"model": self.model_ref, "keep_alive": 0}, timeout=30)
            response.raise_for_status()
        super().unload()
