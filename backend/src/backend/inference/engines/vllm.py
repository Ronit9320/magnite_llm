from __future__ import annotations

import shutil
from typing import AsyncIterator, Optional

import httpx

from backend.inference.engines.base import BaseEngine

DEFAULT_HOST = "127.0.0.1"
DEFAULT_PORT = 8001


class VllmEngine(BaseEngine):
    """Serves a model through a locally-run vLLM OpenAI-compatible server.

    vLLM runs in a separate subprocess and is reached over its HTTP API.
    Requires `vllm` to be installed and an appropriate GPU/VRAM budget.
    """

    name = "vllm"

    def __init__(self, model_ref: str, host: str = DEFAULT_HOST,
                 port: int = DEFAULT_PORT, gpu_memory_utilization: float = 0.8):
        super().__init__(model_ref)
        self.host = host
        self.port = port
        self.gpu_memory_utilization = gpu_memory_utilization
        self._proc: Optional["subprocess.Popen"] = None
        self._base_url = f"http://{host}:{port}/v1"

    @staticmethod
    def is_available() -> bool:
        if not shutil.which("python"):
            return False
        import importlib.util

        return importlib.util.find_spec("vllm") is not None

    def load(self) -> None:
        import subprocess
        import sys

        # Launch vLLM in a subprocess with its OpenAI-compatible server.
        cmd = [
            sys.executable, "-m", "vllm.entrypoints.openai.api_server",
            "--model", self.model_ref,
            "--host", self.host,
            "--port", str(self.port),
            "--gpu-memory-utilization", str(self.gpu_memory_utilization),
            "--served-model-name", self.model_ref,
        ]
        self._proc = subprocess.Popen(
            cmd,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
        # Wait until the server is responsive.
        self._wait_ready(timeout_seconds=240)
        self._loaded = True

    def _wait_ready(self, timeout_seconds: int) -> None:
        import time

        deadline = time.time() + timeout_seconds
        with httpx.Client(timeout=2.0) as client:
            while time.time() < deadline:
                if self._proc is None or self._proc.poll() is not None:
                    raise RuntimeError("vLLM subprocess exited before becoming ready")
                try:
                    r = client.get(f"{self._base_url}/models")
                    if r.status_code == 200:
                        return
                except httpx.HTTPError:
                    pass
                time.sleep(2.0)
        raise RuntimeError("Timed out waiting for vLLM to become ready")

    async def chat_stream(self, messages, **kwargs) -> AsyncIterator[str]:
        payload = {
            "model": self.model_ref,
            "messages": [{"role": m.role.value, "content": m.content} for m in messages],
            "max_tokens": kwargs.get("max_tokens", 256),
            "temperature": kwargs.get("temperature", 0.7),
            "top_p": kwargs.get("top_p", 0.95),
            "stream": True,
        }
        import contextlib

        with contextlib.suppress(Exception):
            with httpx.Client(timeout=None) as client:
                with client.stream(
                    "POST", f"{self._base_url}/chat/completions", json=payload
                ) as resp:
                    resp.raise_for_status()
                    for line in resp.iter_lines():
                        if not line or not line.startswith("data:"):
                            continue
                        data = line[5:].strip()
                        if data == "[DONE]":
                            break
                        import json

                        obj = json.loads(data)
                        delta = obj.get("choices", [{}])[0].get("delta", {})
                        text = delta.get("content")
                        if text:
                            yield text

    async def complete_stream(self, prompt, **kwargs) -> AsyncIterator[str]:
        payload = {
            "model": self.model_ref,
            "prompt": prompt,
            "max_tokens": kwargs.get("max_tokens", 256),
            "temperature": kwargs.get("temperature", 0.7),
            "stream": True,
        }
        import contextlib

        with contextlib.suppress(Exception):
            with httpx.Client(timeout=None) as client:
                with client.stream(
                    "POST", f"{self._base_url}/completions", json=payload
                ) as resp:
                    resp.raise_for_status()
                    for line in resp.iter_lines():
                        if not line or not line.startswith("data:"):
                            continue
                        data = line[5:].strip()
                        if data == "[DONE]":
                            break
                        import json

                        obj = json.loads(data)
                        text = obj.get("choices", [{}])[0].get("text")
                        if text:
                            yield text

    def unload(self) -> None:
        super().unload()
        if self._proc is not None:
            self._proc.terminate()
            try:
                self._proc.wait(timeout=10)
            except Exception:  # noqa: BLE001
                self._proc.kill()
            self._proc = None
