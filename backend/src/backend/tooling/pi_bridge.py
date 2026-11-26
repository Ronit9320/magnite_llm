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
from typing import Callable

from pydantic import TypeAdapter

from backend.inference.schemas import ChatMessage
from backend.inference.supervisor import get_supervisor
from backend.tooling import actions
from backend.tooling.actions import Rejected


_BRIDGE = Path(__file__).with_name("pi") / "worker.mjs"
_MAX_FRAME = 1024 * 1024
_MESSAGES = TypeAdapter(list[ChatMessage])
MAX_TURNS = 6
MAX_PROPOSALS = 4
TURN_TOKENS = 768
# Added to what the model reads, never shown or stored. Small models skip tools without them.
REQUEST_REMINDER = "(Call a tool for this if it needs my computer.)"
STEP_REMINDER = "(If my request has more steps, call the next tool now.)"

# Why an attempt was refused before approval, as told to the model.
REASONS = {
    "not_found": "the file does not exist.",
    "folder_not_found": "the folder does not exist.",
    "not_a_file": "that is not a regular file.",
    "shell_syntax": "shell syntax such as | and > is not supported.",
    "protected_location": "Magnite's own data folder cannot be written to.",
    "invalid_request": "the request was not valid.",
    "action_limit": "the action limit for this task was reached.",
    "unknown_tool": "Magnite does not have that tool.",
    "incomplete_call": "the tool call was incomplete.",
}

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
        "name": "write_file",
        "description": "Create a text file, or replace its contents.",
        "parameters": {"type": "object", "required": ["path", "content"], "properties": {
            "path": {"type": "string", "description": "File path, relative to the working folder or absolute"},
            "content": {"type": "string", "description": "The full text to put in the file"},
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


def _has_shell_operator(command: str) -> bool:
    """Whether the line uses | < > & or ; outside quotes, which needs a shell to mean anything."""
    quote = ""
    escaped = False
    for ch in command:
        if escaped:
            escaped = False
        elif ch == "\\" and quote != "'" and os.name != "nt":
            escaped = True
        elif quote:
            if ch == quote:
                quote = ""
        elif ch in "\"'":
            quote = ch
        elif ch in "|<>&;":
            return True
    return False


def _expand_home(argument: str) -> str:
    prefixes = ("~/", "~\\") if os.name == "nt" else ("~/",)
    return os.path.expanduser(argument) if argument == "~" or argument.startswith(prefixes) else argument


def proposal(call: dict) -> dict:
    """Turn a model tool call into an exact broker action."""
    name, arguments = call.get("name"), call.get("arguments") or {}
    if not isinstance(arguments, dict):
        raise ValueError("Tool arguments must be an object")
    if name == "run_command":
        command = arguments.get("command")
        if not isinstance(command, str):
            raise ValueError("run_command needs a command")
        if _has_shell_operator(command):
            raise Rejected("shell_syntax")
        argv = shlex.split(command, posix=os.name != "nt")
        if not argv:
            raise ValueError("run_command needs a command")
        return {"action": "command", "executable": argv[0],
                "arguments": [_expand_home(item) for item in argv[1:]]}
    if name == "write_file":
        return {"action": "write", "target": arguments.get("path"), "content": arguments.get("content")}
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
        "- To create or change a file, call write_file.",
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
    '{"name":"write_file","arguments":{"path":"notes.txt","content":"text"}}',
    '{"name":"list_folder","arguments":{"path":"."}}',
    '{"name":"open_target","arguments":{"target":"path or URL"}}',
])


def _plain_messages(messages: list[dict], tools: list[dict]) -> list[ChatMessage]:
    """Flatten tool history for models without a native tool format."""
    flat, previous = [], None
    for message in messages:
        role, content = message["role"], message.get("content") or ""
        if role == "system" and tools:
            content = f"{content}\n\n{_JSON_PROTOCOL}"
        elif role == "assistant" and message.get("tool_calls"):
            content = "\n".join(json.dumps(call["function"]) for call in message["tool_calls"])
        elif role == "tool":
            role, content = "user", f"Tool result for {message.get('name', 'the tool')}:\n{content}"
        # Results of several calls in one reply share a turn: many templates reject two user turns in a row.
        if message["role"] == previous == "tool":
            flat[-1]["content"] += f"\n\n{content}"
        else:
            flat.append({"role": role, "content": content})
        previous = message["role"]
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


def _join_command(parts: list[str]) -> str:
    """Quote a command given as a list, leaving items that are shell operators unquoted to be refused.

    A lone ";" is the literal terminator of ``find -exec``.
    """
    find_exec = "-exec" in parts or "-execdir" in parts

    def operator(part: str) -> bool:
        return bool(re.fullmatch(r"\d?[|<>&;]+\d?", part)) and not (part == ";" and find_exec)

    return " ".join(part if operator(part) else shlex.quote(part) for part in parts)


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
        arguments = {"command": _join_command([executable, *map(str, rest)])}
    if name == "read_file" and "target" in arguments:
        arguments = {"path": arguments["target"]}
    if name == "write_file" and "target" in arguments:
        arguments = {"path": arguments["target"], "content": arguments.get("content")}
    return {"name": name, "arguments": arguments}


def _python_calls(text: str) -> list[dict]:
    """Parse ``name(key='value')`` or ``[a(...), b(...)]``, the Llama 3.2 style.

    Empty unless every part of the reply is a call to a tool with text arguments.
    """
    body = text.strip().strip("`").strip()
    if not re.match(r"\[?\s*\w+\s*\(", body):
        return []
    body = body.removeprefix("[").removesuffix("]")
    try:
        items = ast.parse(f"[{body}]", mode="eval").body.elts
        calls = []
        for item in items:
            if not (isinstance(item, ast.Call) and isinstance(item.func, ast.Name)
                    and item.func.id in _TOOL_NAMES and not item.args):
                return []
            arguments = {word.arg: ast.literal_eval(word.value) for word in item.keywords if word.arg}
            if not all(isinstance(value, str) for value in arguments.values()):
                return []
            calls.append({"name": item.func.id, "arguments": arguments})
    except (AttributeError, SyntaxError, TypeError, ValueError):
        return []
    return calls


def _objects(text: str) -> list[dict]:
    """The JSON objects a reply consists of, or none when it holds anything else. A code fence is allowed."""
    rest = re.sub(r"^```(?:json)?\s*|\s*```$", "", text.strip()).strip()
    values = []
    while rest:
        try:
            value, end = json.JSONDecoder().raw_decode(rest)
        except ValueError:
            return []
        if not isinstance(value, dict):
            return []
        values.append(value)
        rest = rest[end:].strip()
    return values


def _named_calls(text: str) -> list[dict]:
    """Parse tool names each on a line followed by their JSON arguments.

    The whole reply must be made of these blocks, so an answer that shows one as an example stays text.
    """
    rest, calls = text.strip(), []
    while rest:
        match = re.match(r"(\w+)[ \t]*\r?\n\s*(?=\{)", rest)
        if not match or match.group(1) not in _TOOL_NAMES:
            return []
        try:
            arguments, end = json.JSONDecoder().raw_decode(rest, match.end())
        except ValueError:
            return []
        call = _call({"name": match.group(1), "arguments": arguments})
        if call is None:
            return []
        calls.append(call)
        rest = rest[end:].strip()
    return calls


def _unique(calls: list[dict]) -> list[dict]:
    """Drop exact repeats within one reply, keeping the order."""
    return list({json.dumps(call, sort_keys=True): call for call in calls}.values())


def parse_reply(raw: str, tools: bool) -> tuple[str, list[dict]]:
    """Split a model turn into answer text and tool calls."""
    text = _strip_thinking(raw)
    tagged = re.findall(r"<tool_call>\s*(.*?)\s*(?:</tool_call>|$)", text, flags=re.S)
    if tagged:
        answer = re.sub(r"<tool_call>.*?(?:</tool_call>|$)", "", text, flags=re.S).strip()
        if not tools:
            return answer, []
        # One call per block: _json_values can read the same object from a block twice.
        found = (next(filter(None, map(_call, _json_values(block))), None) for block in tagged)
        calls = _unique([call for call in found if call])
        if calls:
            return answer, calls
        return text, []
    if tools:
        if calls := _python_calls(text) or _named_calls(text):
            return "", _unique(calls)
        # One object is read below, which also allows words before it. Several must be the whole reply.
        values = _objects(text)
        if len(values) > 1 and all(calls := [_call(value) for value in values]):
            return "", _unique(calls)
    for value in _json_values(text):
        if tools and (call := _call(value)):
            return "", [call]
        if set(value) == {"answer"} and isinstance(value["answer"], str):
            return value["answer"].strip(), []
        break
    # Small models often preface a direct answer with this sentence.
    trimmed = re.sub(r"^(None of the tools|No tools?) (are|is) (needed|required)[^.]*\.\s*", "", text, flags=re.I)
    return trimmed or text, []


_TAGGED = r"<tool_call>\s*(.*?)\s*(?:</tool_call>|$)"


def call_problem(raw: str, offered: bool) -> tuple[str, str] | None:
    """Why a reply that tried to call a tool cannot run, as (reason, tool name). None for an answer.

    Only a tagged call, or a reply that is nothing but JSON, counts as an attempt.
    """
    text = _strip_thinking(raw)
    tagged = re.findall(_TAGGED, text, flags=re.S)
    if not tagged and not offered and (calls := _named_calls(text)):
        return "action_limit", calls[0]["name"]
    body = tagged[0] if tagged else re.sub(r"^```(?:json)?\s*|\s*```$", "", text).strip()
    if not tagged and not body.startswith("{"):
        return None
    value = next(_json_values(body), None) if tagged else None
    if not tagged:
        # Read the leading object, so words after an answer do not make it look broken.
        with suppress(ValueError):
            value, _ = json.JSONDecoder().raw_decode(body)
    if not tagged:
        # A run of calls is refused as a whole when one of them cannot run, so name that one.
        values = _objects(text)
        if len(values) > 1 and all("name" in item and "arguments" in item for item in values):
            value = next((item for item in values if _call(item) is None), value)
    if not isinstance(value, dict):
        cut_off = tagged or re.match(r'\{\s*"name"\s*:\s*"[^"]*"\s*,\s*"(?:arguments|parameters)"', body)
        return ("incomplete_call", "") if cut_off else None
    name = value.get("name")
    if not tagged and not (isinstance(name, str) and ("arguments" in value or "parameters" in value)):
        return None
    if not isinstance(name, str):
        return "incomplete_call", ""
    name = name[:64].encode("utf-8", "replace").decode("utf-8")
    if name not in _TOOL_NAMES:
        return "unknown_tool", name
    if not offered:
        return "action_limit", name
    return None if _call(value) else ("incomplete_call", name)


def _problem_message(reason: str, name: str) -> str:
    if reason == "unknown_tool":
        return f"Nothing ran: the model asked for a tool Magnite does not have ({name})."
    if reason == "action_limit":
        return "Nothing ran: the action limit for this task was reached."
    return "Nothing ran: the model's tool call was incomplete."


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


_TRIM_MARK = "\n[... truncated to fit the model's context ...]"
_TOO_LONG = "The tool conversation is longer than the loaded model's context"


def _fit_tool_output(messages: list[dict], size: Callable[[list[dict]], int], limit: int) -> list[dict]:
    """Return a copy of ``messages`` within ``limit``, shortening the longest tool results first."""
    fitted = [dict(message) for message in messages]
    suffix = f"\n\n{STEP_REMINDER}"
    bodies = {index: (message["content"] or "").removesuffix(suffix)
              for index, message in enumerate(fitted) if message["role"] == "tool"}
    suffixes = {index: suffix if (fitted[index]["content"] or "").endswith(suffix) else "" for index in bodies}
    while size(fitted) > limit:
        longest = max((index for index in bodies if len(bodies[index]) > 400),
                      key=lambda index: len(bodies[index]), default=None)
        if longest is None:
            raise ValueError(_TOO_LONG)
        bodies[longest] = bodies[longest][:len(bodies[longest]) // 2]
        fitted[longest]["content"] = bodies[longest] + _TRIM_MARK + suffixes[longest]
    return fitted


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
            def prompt_for(items):
                prompt = active.render_tool_prompt(items, TOOLS)
                if prompt is None:
                    raise ValueError("The model's chat template stopped rendering tools")
                return prompt

            fitted = _fit_tool_output(messages, lambda items: active.count_tokens(prompt_for(items)),
                                      active.context_size() - TURN_TOKENS)
            return prompt_for(fitted)

        stream = supervisor.complete_stream(None, prepare=render, max_tokens=TURN_TOKENS,
                                            temperature=0, templated=True)
    else:
        def check(active):
            verify(active)
            fitted = _fit_tool_output(messages,
                                      lambda items: active.count_messages(_plain_messages(items, tools)),
                                      active.context_size() - TURN_TOKENS)
            return _plain_messages(fitted, tools)

        stream = supervisor.chat_stream(None, prepare=check, max_tokens=TURN_TOKENS,
                                        temperature=0, tool_mode=True)
    return asyncio.run(_stream_text(stream, stop))


def _context_window(engine) -> int:
    try:
        return int(engine.context_size())
    except (AttributeError, RuntimeError, ValueError, TypeError):
        return 8192


def _hit_token_cap(engine, raw: str) -> bool:
    """Whether a reply ran into the per-turn token cap. Engines stream text without a finish reason."""
    try:
        return engine.count_tokens(raw) >= TURN_TOKENS
    except (AttributeError, RuntimeError, ValueError):
        return False


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
        return f"The action was not run: {REASONS.get(result.get('outcome'), REASONS['invalid_request'])}"
    if status not in ("completed", "failed"):
        return f"The action was not run (status: {status})."
    lines = [f"Status: {result.get('outcome', status)}"]
    if "exit_code" in result:
        lines.append(f"Exit code: {result['exit_code']}")
    if "output" in result:
        lines.append("Output:\n" + (result["output"].strip() or "(no output)"))
    return "\n".join(lines)


def run(value, engine, approve, stop: Event, *, instructions: str = "",
        working_directory: str | None = None,
        reject: Callable[[str, dict, str], None] | None = None) -> str:
    """Run one agent task. ``approve`` is None for a text-only agent without tools.

    ``reject`` records an attempt refused here; ``approve`` records the ones it refuses itself.
    """
    messages = chat_messages(value)
    executable = node_path()
    native = approve is not None and native_tools(engine)
    system = system_prompt(working_directory) if approve is not None else instructions
    history = [{"role": "system", "content": system}] if system else []
    history += [message.model_dump(mode="json") for message in messages if message.role.value != "system"]
    if approve is not None:
        history[-1]["content"] += f"\n\n{REQUEST_REMINDER}"
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
                  "maxTurns": MAX_TURNS, "maxTokens": TURN_TOKENS, "contextWindow": _context_window(engine)})
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
                        if not calls and _hit_token_cap(engine, raw):
                            if "<tool_call>" in raw or _strip_thinking(raw).startswith("{"):
                                raise RuntimeError(f"The model's reply was cut off at {TURN_TOKENS} tokens "
                                                   "before its tool call finished")
                            text = f"{text}\n\n[The reply was cut off at {TURN_TOKENS} tokens.]".strip()
                        if approve is not None and not calls and (problem := call_problem(raw, bool(tools))):
                            # Replace the markup, keeping any words the model wrote around it.
                            prose = (re.sub(_TAGGED, "", _strip_thinking(raw), flags=re.S).strip()
                                     if "<tool_call>" in raw else "")
                            text = f"{prose}\n\n{_problem_message(*problem)}".strip()
                            if reject is not None:
                                reject("unknown", {"action": "unknown", "target": problem[1]}, problem[0])
                        if calls and reject is not None:
                            # A call that cannot run, beside ones that can, still gets its "Not run" card.
                            for block in re.findall(_TAGGED, _strip_thinking(raw), flags=re.S):
                                if problem := call_problem(f"<tool_call>{block}</tool_call>", True):
                                    reject("unknown", {"action": "unknown", "target": problem[1]}, problem[0])
                        if not text and not calls:
                            text = "The model returned an empty reply."
                        send({"type": "generation", "text": text, "calls": calls})
                    case "proposal":
                        if approve is None:
                            raise ValueError("This Pi agent does not have action tools")
                        proposals += 1
                        call, request, reason = frame["proposal"], None, "action_limit"
                        if proposals <= MAX_PROPOSALS:
                            try:
                                request = proposal(call)
                            except Rejected as exc:
                                reason = exc.reason
                            except (ValueError, OSError, TypeError):
                                reason = "invalid_request"
                        if request is None:
                            if reject is not None:
                                reject(*actions.describe(call), reason)
                            result = {"status": "rejected", "outcome": reason}
                        else:
                            try:
                                result = approve(request)
                            except Rejected as exc:
                                result = {"status": "rejected", "outcome": exc.reason}
                            except (ValueError, OSError, TypeError):
                                result = {"status": "rejected", "outcome": "invalid_request"}
                        receipt = receipt_text(result)
                        completed = result.get("status") == "completed"
                        # Never after a refusal or failure, which would push the model to try again.
                        if completed and proposals < MAX_PROPOSALS:
                            receipt += f"\n\n{STEP_REMINDER}"
                        send({"type": "receipt", "text": receipt, "error": not completed})
                    case "result":
                        if not isinstance(frame["text"], str) or not frame["text"].strip():
                            raise ValueError("Pi returned an empty answer")
                        answer = frame["text"]
                        if approve is not None and (REQUEST_REMINDER in answer or STEP_REMINDER in answer):
                            answer = answer.replace(REQUEST_REMINDER, "").replace(STEP_REMINDER, "").strip()
                        return answer or "The model returned an empty reply."
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
