"""Run Pi's agent core on stdio and supply Magnite's local chat as its provider."""

from __future__ import annotations

import asyncio
from contextlib import aclosing, suppress
import json
import os
from pathlib import Path
from queue import Empty, Full, Queue
import re
import shutil
import subprocess
from threading import Event, Thread
from time import monotonic

from pydantic import TypeAdapter

from backend.inference.schemas import ChatMessage
from backend.inference.supervisor import get_supervisor


_BRIDGE = Path(__file__).with_name("pi") / "worker.mjs"
_MAX_FRAME = 1024 * 1024
_MESSAGES = TypeAdapter(list[ChatMessage])


def node_path() -> str:
    executable = shutil.which("node")
    if not executable or not (_BRIDGE.parent / "node_modules" / "@earendil-works" / "pi-agent-core").is_dir():
        raise ValueError("Pi requires Node 22.19 or later and the locked Pi bridge dependencies")
    try:
        result = subprocess.run([executable, "--version"], capture_output=True, encoding="utf-8", timeout=3)
    except subprocess.TimeoutExpired as exc:
        raise ValueError("Node did not respond to the Pi runtime check") from exc
    version = re.fullmatch(r"v(\d+)\.(\d+)\.(\d+)", result.stdout.strip())
    if result.returncode or version is None or tuple(map(int, version.groups())) < (22, 19, 0):
        raise ValueError("Pi requires Node 22.19 or later")
    return executable


def loaded_engine():
    node_path()
    engine = get_supervisor().current_engine()
    if engine.name not in ("torch", "llamacpp", "vllm"):
        raise ValueError("Pi requires a loaded Torch, llama.cpp, or local vLLM chat model")
    return engine


def chat_messages(value) -> list[ChatMessage]:
    if isinstance(value, str):
        value = [{"role": "user", "content": value}]
    messages = _MESSAGES.validate_python(value)
    if (not messages or len(messages) > 64 or messages[-1].role.value != "user"
            or not messages[-1].content.strip() or sum(len(item.content) for item in messages) > 128_000):
        raise ValueError("Pi needs a nonempty user turn and at most 64 messages or 128,000 characters")
    return messages


async def _local_reply(messages, engine, stop: Event) -> str:
    def verify_loaded(active):
        if active is not engine:
            raise ValueError("The chat model changed during the tool run")
        if active.count_messages(messages) + 512 > active.context_size():
            raise ValueError("Pi tool conversation exceeds the loaded model's context")
        return messages

    async def collect():
        parts = []
        async with aclosing(get_supervisor().chat_stream(
            messages, max_tokens=512, temperature=0, tool_mode=True, prepare=verify_loaded,
        )) as stream:
            async for delta in stream:
                parts.append(delta)
        return "".join(parts)

    task = asyncio.create_task(collect())
    try:
        deadline = monotonic() + 90
        while not stop.is_set() and monotonic() < deadline:
            done, _ = await asyncio.wait((task,), timeout=0.1)
            if done:
                return task.result()
        raise RuntimeError("Pi's local model turn was cancelled or timed out")
    finally:
        if not task.done():
            task.cancel()
            with suppress(asyncio.CancelledError):
                await task


def run(value, engine, approve, stop: Event) -> str:
    messages = chat_messages(value)
    executable = node_path()
    # The bridge uses pipes and only needs the host's runtime environment.
    # Omit Node injection settings and provider credentials.
    env = {key: os.environ[key] for key in ("PATH", "SystemRoot", "WINDIR", "TEMP", "TMP", "LANG")
           if key in os.environ}
    with subprocess.Popen([executable, str(_BRIDGE)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.DEVNULL, env=env, cwd=_BRIDGE.parent) as process:
        frames: Queue = Queue(maxsize=8)
        finished = Event()

        def read_frames():
            while not finished.is_set():
                line = process.stdout.readline(_MAX_FRAME + 1)
                if not line or len(line) > _MAX_FRAME:
                    break
                try:
                    frames.put_nowait(line)
                except Full:
                    break

        reader = Thread(target=read_frames, name="pi-stdio", daemon=True)
        reader.start()

        def send(frame):
            data = json.dumps(frame, ensure_ascii=True).encode()
            if len(data) > _MAX_FRAME:
                raise ValueError("Pi bridge input exceeds its size limit")
            process.stdin.write(data + b"\n")
            process.stdin.flush()

        try:
            send({"type": "start", "messages": [message.model_dump(mode="json") for message in messages]})
            turns = proposals = 0
            deadline = monotonic() + 900
            while not stop.is_set() and monotonic() < deadline:
                try:
                    frame = json.loads(frames.get(timeout=0.1))
                except Empty:
                    if not reader.is_alive():
                        raise RuntimeError("Pi bridge exited before returning an answer")
                    continue
                match frame["type"]:
                    case "generate":
                        turns += 1
                        if turns > 5:
                            raise RuntimeError("Pi exceeded its turn budget")
                        prompt = _MESSAGES.validate_python(frame["messages"])
                        reply = asyncio.run(_local_reply(prompt, engine, stop))
                        send({"type": "generation", "text": reply})
                    case "proposal":
                        proposals += 1
                        if proposals > 4:
                            raise RuntimeError("Pi exceeded its action budget")
                        try:
                            result = approve(frame["proposal"])
                        except (ValueError, OSError, TypeError):
                            result = {"status": "rejected", "outcome": "invalid_action"}
                        send({"type": "receipt", "result": result})
                    case "result":
                        if not isinstance(frame["text"], str) or not frame["text"].strip():
                            raise ValueError("Pi returned an empty answer")
                        return frame["text"]
                    case "error":
                        raise RuntimeError(frame["message"])
                    case _:
                        raise ValueError("Invalid Pi bridge frame")
            raise RuntimeError("Pi run was cancelled or timed out")
        finally:
            finished.set()
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=2)
                except subprocess.TimeoutExpired:
                    process.kill()
            reader.join(timeout=2)
