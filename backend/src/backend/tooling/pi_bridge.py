"""Run Pi's agent core on stdio and supply Magnite's local chat as its provider.

Pi owns the agent loop: it sequences model turns and tool executions. Magnite owns
everything that touches the model and the computer. This module renders each turn
with the loaded model's own tool-calling chat template when it has one (Qwen,
Llama 3.x, SmolLM3 and similar), falls back to a plain JSON protocol otherwise,
parses the reply into text or tool calls, and sends every action through the
approval broker.
"""

from __future__ import annotations

import ast
import asyncio
from contextlib import aclosing, suppress
import json
import os
from pathlib import Path
from queue import Empty, Full, Queue
import re
import shlex
import shutil
import sys
import subprocess
from threading import Event, Thread
from time import monotonic

from pydantic import TypeAdapter

from backend.inference.schemas import ChatMessage
from backend.inference.supervisor import get_supervisor


_BRIDGE = Path(__file__).with_name("pi") / "worker.mjs"
_MAX_FRAME = 1024 * 1024
_MESSAGES = TypeAdapter(list[ChatMessage])
MAX_TURNS = 6
MAX_PROPOSALS = 4
TURN_TOKENS = 768

# OpenAI-style declarations the model sees. ``proposal`` maps each call to a broker action.
TOOLS = [
    {"type": "function", "function": {
        "name": "run_command",
        "description": "Run a command line and get its exit code and output, for example: python3 --version",
        "parameters": {"type": "object", "required": ["command"], "properties": {
            "command": {"type": "string", "description": "The full command line to run"},
        }},
    }},
    {"type": "function", "function": {
        "name": "read_file",
        "description": "Read a text file and get its contents.",
        "parameters": {"type": "object", "required": ["path"], "properties": {
            "path": {"type": "string", "description": "File path, relative to the working folder or absolute"},
        }},
    }},
    {"type": "function", "function": {
        "name": "list_folder",
        "description": "List the files and folders inside a folder.",
        "parameters": {"type": "object", "properties": {
            "path": {"type": "string", "description": "Folder path; omit it for the working folder"},
        }},
    }},
    {"type": "function", "function": {
        "name": "open_target",
        "description": "Open a file, folder or http(s) URL in the user's default application.",
        "parameters": {"type": "object", "required": ["target"], "properties": {
            "target": {"type": "string", "description": "Path or URL to open"},
        }},
    }},
]
_TOOL_NAMES = {tool["function"]["name"] for tool in TOOLS}
_LEGACY = {"command": "run_command", "open": "open_target", "read": "read_file"}


def proposal(call: dict) -> dict:
    """Turn a model tool call into an exact broker action."""
    name, arguments = call.get("name"), call.get("arguments") or {}
    if not isinstance(arguments, dict):
        raise ValueError("Tool arguments must be an object")
    if name == "run_command":
        command = arguments.get("command")
        if not isinstance(command, str):
            raise ValueError("run_command needs a command")
        argv = shlex.split(command, posix=os.name != "nt")
        if not argv:
            raise ValueError("run_command needs a command")
        return {"action": "command", "executable": argv[0], "arguments": argv[1:]}
    if name in ("read_file", "list_folder"):
        return {"action": "read", "target": arguments.get("path") or "."}
    if name == "open_target":
        return {"action": "open", "target": arguments.get("target")}
    raise ValueError("Unknown tool")


def _node_environment() -> dict[str, str]:
    return {key: os.environ[key] for key in ("PATH", "SystemRoot", "WINDIR", "TEMP", "TMP", "LANG")
            if key in os.environ}


def node_path() -> str:
    prefix = Path(sys.prefix)
    if prefix.name == "python" and prefix.parent.name == "runtime":
        bundled = prefix.parent / "node" / ("node.exe" if sys.platform == "win32" else "node")
        executable = str(bundled) if bundled.is_file() else None
    else:
        executable = shutil.which("node")
    if not executable:
        raise ValueError("Tool mode needs Node.js 22.19 or later on PATH")
    if not (_BRIDGE.parent / "node_modules" / "@earendil-works" / "pi-agent-core").is_dir():
        raise ValueError("Tool mode needs the Pi bridge packages. Run: npm ci --ignore-scripts "
                         "--prefix backend/src/backend/tooling/pi")
    try:
        result = subprocess.run([executable, "--version"], capture_output=True, encoding="utf-8",
                                env=_node_environment(), timeout=3)
    except subprocess.TimeoutExpired as exc:
        raise ValueError("Node did not respond to the Pi runtime check") from exc
    version = re.fullmatch(r"v(\d+)\.(\d+)\.(\d+)", result.stdout.strip())
    if result.returncode or version is None or tuple(map(int, version.groups())) < (22, 19, 0):
        raise ValueError("Tool mode needs Node.js 22.19 or later")
    return executable


def loaded_engine():
    node_path()
    engine = get_supervisor().current_engine()
    if engine.name not in ("torch", "llamacpp", "vllm"):
        raise ValueError("Tool mode needs a model loaded with the Torch, llama.cpp, or local vLLM engine")
    return engine


def chat_messages(value) -> list[ChatMessage]:
    if isinstance(value, str):
        value = [{"role": "user", "content": value}]
    messages = _MESSAGES.validate_python(value)
    if (not messages or len(messages) > 64 or messages[-1].role.value != "user"
            or not messages[-1].content.strip() or sum(len(item.content) for item in messages) > 128_000):
        raise ValueError("Pi needs a nonempty user turn and at most 64 messages or 128,000 characters")
    return messages


def native_tools(engine) -> bool:
    """Whether the loaded model's chat template has its own tool-calling format."""
    probe = [{"role": "user", "content": "ping"}]
    try:
        return engine.render_tool_prompt(probe, TOOLS) is not None
    except (AttributeError, RuntimeError, ValueError):
        return False


_DISK = "wmic logicaldisk get size,freespace,caption" if sys.platform == "win32" else "df -h"


def system_prompt(directory: str | None) -> str:
    platform = {"win32": "Windows", "darwin": "macOS"}.get(sys.platform, "Linux")
    lines = [
        f"You are Magnite's local assistant, running on the user's {platform} computer.",
        "You have access to the user's files, folders and programs through your tools. Do not say you lack "
        "access: call a tool instead. The user approves each tool call before it runs.",
        "- To answer questions about the user's files or folders, call read_file or list_folder.",
        "- Use run_command to run programs or check the system. Never guess or invent a result.",
        "- Use open_target to open a file, folder, or http(s) URL for the user.",
        "- Commands run without a shell, so pipes, redirects, wildcards, and shell built-ins are unavailable.",
        "- After a tool result, answer the user in plain language based on that result. If an action was "
        "denied or failed, say so and do not request it again.",
        "- If no tool is needed, answer the question directly without mentioning tools.",
        "Example tool calls:",
        'what is in todo.txt -> {"name": "read_file", "arguments": {"path": "todo.txt"}}',
        f'how much disk space is free -> {{"name": "run_command", "arguments": {{"command": "{_DISK}"}}}}',
        'which files are here -> {"name": "list_folder", "arguments": {}}',
    ]
    if directory:
        lines.insert(2, f"Commands run in the folder {directory}.")
    return "\n".join(lines)


_JSON_PROTOCOL = "\n".join([
    "Reply with exactly one JSON object and nothing else, in one of these forms:",
    '{"answer":"your final answer to the user"}',
    '{"name":"run_command","arguments":{"command":"program --flag"}}',
    '{"name":"read_file","arguments":{"path":"notes.txt"}}',
    '{"name":"list_folder","arguments":{"path":"."}}',
    '{"name":"open_target","arguments":{"target":"path or URL"}}',
])


def _plain_messages(messages: list[dict], tools: list[dict]) -> list[ChatMessage]:
    """Flatten tool history for models without a native tool format."""
    flat = []
    for message in messages:
        role, content = message["role"], message.get("content") or ""
        if role == "system" and tools:
            content = f"{content}\n\n{_JSON_PROTOCOL}"
        elif role == "assistant" and message.get("tool_calls"):
            content = "\n".join(json.dumps(call["function"]) for call in message["tool_calls"])
        elif role == "tool":
            role, content = "user", f"Tool result for {message.get('name', 'the tool')}:\n{content}"
        flat.append({"role": role, "content": content})
    return _MESSAGES.validate_python(flat)


def _strip_thinking(text: str) -> str:
    text = re.sub(r"<think>.*?</think>", "", text, flags=re.S)
    # A reply cut off inside its thinking has no usable answer.
    return re.sub(r"<think>.*", "", text, flags=re.S).strip()


def _json_values(text: str):
    """Yield JSON objects from fenced blocks, then from the whole text, then from the first {...}."""
    candidates = re.findall(r"```(?:json)?\s*(.*?)```", text, flags=re.S) + [text]
    start, end = text.find("{"), text.rfind("}")
    if 0 <= start < end:
        candidates.append(text[start:end + 1])
    for candidate in candidates:
        with suppress(ValueError):
            value = json.loads(candidate.strip())
            if isinstance(value, dict):
                yield value


def _call(value) -> dict | None:
    if not isinstance(value, dict):
        return None
    name, arguments = value.get("name"), value.get("arguments", value.get("parameters", {}))
    if isinstance(arguments, str):
        with suppress(ValueError):
            arguments = json.loads(arguments)
    action = value.get("action")
    if name not in _TOOL_NAMES and action in _LEGACY:  # The earlier {"action": ...} protocol.
        name, arguments = _LEGACY[action], {k: v for k, v in value.items() if k != "action"}
    if name not in _TOOL_NAMES or not isinstance(arguments, dict):
        return None
    executable, rest = arguments.get("executable"), arguments.get("arguments", [])
    if name == "run_command" and isinstance(executable, str) and isinstance(rest, list):
        arguments = {"command": shlex.join([executable, *map(str, rest)])}
    if name == "read_file" and "target" in arguments:
        arguments = {"path": arguments["target"]}
    return {"name": name, "arguments": arguments}


def _python_call(text: str) -> dict | None:
    """Parse ``name(key='value')`` or ``[name(key='value')]``, the Llama 3.2 style."""
    match = re.fullmatch(r"\[?\s*(\w+)\s*\((.*)\)\s*\]?", text.strip().strip("`"), flags=re.S)
    if not match or match.group(1) not in _TOOL_NAMES:
        return None
    try:
        call = ast.parse(f"f({match.group(2)})", mode="eval").body
        arguments = {item.arg: ast.literal_eval(item.value) for item in call.keywords if item.arg}
    except (SyntaxError, ValueError):
        return None
    if call.args or not all(isinstance(value, str) for value in arguments.values()):
        return None
    return {"name": match.group(1), "arguments": arguments}


def parse_reply(raw: str, tools: bool) -> tuple[str, list[dict]]:
    """Split a model turn into answer text and tool calls."""
    text = _strip_thinking(raw)
    tagged = re.findall(r"<tool_call>\s*(.*?)\s*(?:</tool_call>|$)", text, flags=re.S)
    if tagged:
        answer = re.sub(r"<tool_call>.*?(?:</tool_call>|$)", "", text, flags=re.S).strip()
        if not tools:
            return answer, []
        calls = [call for block in tagged for call in map(_call, _json_values(block)) if call][:1]
        if calls:
            return answer, calls
        return text, []
    if tools and (call := _python_call(text)):
        return "", [call]
    for value in _json_values(text):
        if tools and (call := _call(value)):
            return "", [call]
        if set(value) == {"answer"} and isinstance(value["answer"], str):
            return value["answer"].strip(), []
        break
    # Small models often preface a direct answer with this sentence.
    trimmed = re.sub(r"^(None of the tools|No tools?) (are|is) (needed|required)[^.]*\.\s*", "", text, flags=re.I)
    return trimmed or text, []


async def _stream_text(stream, stop: Event) -> str:
    async def collect():
        parts = []
        async with aclosing(stream) as items:
            async for delta in items:
                parts.append(delta)
        return "".join(parts)

    task = asyncio.create_task(collect())
    try:
        deadline = monotonic() + 120
        while not stop.is_set() and monotonic() < deadline:
            done, _ = await asyncio.wait((task,), timeout=0.1)
            if done:
                return task.result()
        raise RuntimeError("The local model turn was cancelled or timed out")
    finally:
        if not task.done():
            task.cancel()
            with suppress(asyncio.CancelledError):
                await task


def _generate(messages: list[dict], tools: list[dict], engine, stop: Event, native: bool) -> str:
    """Run one model turn on the loaded chat engine and return its raw text."""
    supervisor = get_supervisor()

    def verify(active):
        if active is not engine:
            raise ValueError("The chat model changed during the tool run")

    if native:
        def render(active):
            verify(active)
            # Keep the same declarations once the action budget is spent so earlier
            # tool turns still render; parse_reply then ignores further calls.
            prompt = active.render_tool_prompt(messages, TOOLS)
            if prompt is None:
                raise ValueError("The model's chat template stopped rendering tools")
            if active.count_tokens(prompt) + TURN_TOKENS > active.context_size():
                raise ValueError("The tool conversation is longer than the loaded model's context")
            return prompt

        stream = supervisor.complete_stream(None, prepare=render, max_tokens=TURN_TOKENS,
                                            temperature=0, templated=True)
    else:
        plain = _plain_messages(messages, tools)

        def check(active):
            verify(active)
            if active.count_messages(plain) + TURN_TOKENS > active.context_size():
                raise ValueError("The tool conversation is longer than the loaded model's context")
            return plain

        stream = supervisor.chat_stream(None, prepare=check, max_tokens=TURN_TOKENS,
                                        temperature=0, tool_mode=True)
    return asyncio.run(_stream_text(stream, stop))


def receipt_text(result: dict) -> str:
    """Describe an action outcome for the model."""
    status = result.get("status")
    if status == "denied":
        return "The user denied this action, so it did not run."
    if status == "expired":
        return "The user did not answer the approval in time, so the action was not run."
    if result.get("outcome") == "action_limit":
        return "The action limit for this task is reached, so the action was not run. Answer the user now."
    if status == "rejected":
        return f"The action was invalid and was not run ({result.get('outcome', 'invalid_action')})."
    if status not in ("completed", "failed"):
        return f"The action was not run (status: {status})."
    lines = [f"Status: {result.get('outcome', status)}"]
    if "exit_code" in result:
        lines.append(f"Exit code: {result['exit_code']}")
    if "output" in result:
        lines.append("Output:\n" + (result["output"].strip() or "(no output)"))
    return "\n".join(lines)


def run(value, engine, approve, stop: Event, *, instructions: str = "",
        working_directory: str | None = None) -> str:
    """Run one agent task. ``approve`` is None for a text-only agent without tools."""
    messages = chat_messages(value)
    executable = node_path()
    native = approve is not None and native_tools(engine)
    system = system_prompt(working_directory) if approve is not None else instructions
    history = [{"role": "system", "content": system}] if system else []
    history += [message.model_dump(mode="json") for message in messages if message.role.value != "system"]
    # The bridge uses pipes and only needs the host's runtime environment.
    # Omit Node injection settings and provider credentials.
    with subprocess.Popen([executable, str(_BRIDGE)], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, env=_node_environment(), cwd=_BRIDGE.parent) as process:
        frames: Queue = Queue(maxsize=8)
        finished = Event()
        errors: list[bytes] = []

        def read_frames():
            while not finished.is_set():
                line = process.stdout.readline(_MAX_FRAME + 1)
                if not line or len(line) > _MAX_FRAME:
                    break
                try:
                    frames.put_nowait(line)
                except Full:
                    break

        def read_errors():
            errors.append(process.stderr.read(4096))
            process.stderr.read()

        reader = Thread(target=read_frames, name="pi-stdio", daemon=True)
        reader.start()
        Thread(target=read_errors, name="pi-stderr", daemon=True).start()

        def send(frame):
            data = json.dumps(frame, ensure_ascii=True).encode()
            if len(data) > _MAX_FRAME:
                raise ValueError("Pi bridge input exceeds its size limit")
            process.stdin.write(data + b"\n")
            process.stdin.flush()

        try:
            send({"type": "start", "messages": history, "tools": TOOLS if approve is not None else [],
                  "maxTurns": MAX_TURNS})
            turns = proposals = 0
            deadline = monotonic() + 900
            while not stop.is_set() and monotonic() < deadline:
                try:
                    frame = json.loads(frames.get(timeout=0.1))
                except Empty:
                    if not reader.is_alive():
                        detail = b"".join(errors).decode("utf-8", "replace").strip().splitlines()
                        raise RuntimeError("The Pi bridge stopped before answering"
                                           + (f": {detail[-1][:300]}" if detail else ""))
                    continue
                match frame["type"]:
                    case "generate":
                        turns += 1
                        if turns > MAX_TURNS:
                            raise RuntimeError("Pi exceeded its turn budget")
                        tools = frame.get("tools") or []
                        raw = _generate(frame["messages"], tools, engine, stop, native)
                        text, calls = parse_reply(raw, bool(tools) and approve is not None)
                        if not text and not calls:
                            text = "The model returned an empty reply."
                        send({"type": "generation", "text": text, "calls": calls})
                    case "proposal":
                        if approve is None:
                            raise ValueError("This Pi agent does not have action tools")
                        proposals += 1
                        if proposals > MAX_PROPOSALS:
                            result = {"status": "rejected", "outcome": "action_limit"}
                        else:
                            try:
                                result = approve(proposal(frame["proposal"]))
                            except (ValueError, OSError, TypeError):
                                result = {"status": "rejected", "outcome": "invalid_action"}
                        send({"type": "receipt", "text": receipt_text(result),
                              "error": result.get("status") != "completed"})
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
