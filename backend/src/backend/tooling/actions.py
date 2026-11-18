"""Validate an exact local action and dispatch it after the broker claims approval."""

from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
import shutil
import signal
import subprocess
from threading import Event, Thread
from time import monotonic
from typing import Annotated, Literal
from urllib.parse import urlsplit

from pydantic import AfterValidator, BaseModel, ConfigDict, Field, TypeAdapter

from backend.textcheck import ensure_utf8


def _argument(value: str) -> str:
    ensure_utf8(value, "Tool argument")
    if any(ord(ch) < 32 or ord(ch) == 127 for ch in value):
        raise ValueError("Tool arguments must be single-line text")
    return value


Argument = Annotated[str, Field(max_length=4096), AfterValidator(_argument)]
Target = Annotated[Argument, Field(min_length=1)]


class OpenAction(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, frozen=True)
    action: Literal["open"]
    target: Target


class ReadAction(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, frozen=True)
    action: Literal["read"]
    target: Target


class CommandAction(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, frozen=True)
    action: Literal["command"]
    executable: Target
    arguments: list[Argument] = Field(max_length=32)


PROPOSAL = TypeAdapter(Annotated[OpenAction | ReadAction | CommandAction, Field(discriminator="action")])


@dataclass(frozen=True)
class PathIdentity:
    path: Path
    identity: tuple[int, ...]

    @classmethod
    def read(cls, path: Path):
        info = path.stat()
        stable = (info.st_dev, info.st_ino, info.st_mode)
        if path.is_file():
            stable += (info.st_size, info.st_mtime_ns)
        return cls(path, stable)

    def verify(self) -> None:
        if self.path.resolve(strict=True) != self.path or PathIdentity.read(self.path) != self:
            raise ValueError("An approved path changed before dispatch")


@dataclass(frozen=True)
class PreparedAction:
    action: Literal["open", "read", "command"]
    target: str
    arguments: tuple[str, ...]
    directory: Path
    paths: tuple[PathIdentity, ...]

    def display(self) -> dict:
        if self.action in ("open", "read"):
            return {"action": self.action, "target": self.target}
        return {"action": "command", "executable": self.target, "arguments": list(self.arguments),
                "working_directory": str(self.directory)}


def resolve_directory(value: str) -> Path:
    path = Path(_argument(value)).expanduser()
    if not path.is_absolute():
        raise ValueError("Working directory must be absolute")
    path = path.resolve(strict=True)
    if not path.is_dir():
        raise ValueError("Working directory must be an existing folder")
    return path


def _resolve_path(value: str, directory: Path) -> Path:
    return (directory / Path(value).expanduser()).resolve(strict=True)


def prepare(proposal, directory: Path) -> PreparedAction:
    action = PROPOSAL.validate_python(proposal)
    if isinstance(action, ReadAction):
        path = _resolve_path(action.target, directory)
        if not (path.is_file() or path.is_dir()):
            raise ValueError("Read requires a regular file or folder")
        return PreparedAction("read", str(path), (), directory, (PathIdentity.read(path),))
    if isinstance(action, OpenAction):
        value = action.target
        path = Path(value)
        if not path.is_absolute() and ":" in value.split("/", 1)[0]:
            parsed = urlsplit(value)
            if (parsed.scheme not in ("http", "https") or not parsed.hostname
                    or parsed.username is not None or parsed.password is not None
                    or any(ch.isspace() or ch == "\\" for ch in value)):
                raise ValueError("Open URLs must be HTTP(S) addresses without credentials")
            # Accessing port checks its syntax and range.
            parsed.port
            return PreparedAction("open", value, (), directory, ())
        path = _resolve_path(value, directory)
        if not (path.is_file() or path.is_dir()):
            raise ValueError("Open requires a regular file or folder")
        return PreparedAction("open", str(path), (), directory, (PathIdentity.read(path),))

    raw = Path(action.executable)
    if raw.name == action.executable:
        program = shutil.which(action.executable)
        if program is None:
            raise ValueError("Requested executable is unavailable")
        raw = Path(program).absolute()
    executable = _resolve_path(str(raw), directory)
    if not executable.is_file() or not os.access(executable, os.X_OK):
        raise ValueError("Command requires an executable file")
    if os.name == "nt" and executable.suffix.lower() != ".exe":
        raise ValueError("Commands on Windows require a native .exe")
    paths = (PathIdentity.read(executable), PathIdentity.read(directory))
    return PreparedAction("command", str(executable), tuple(action.arguments), directory, paths)


def _kill_command(process: subprocess.Popen) -> None:
    if os.name != "nt":
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    else:
        import psutil

        try:
            for child in psutil.Process(process.pid).children(recursive=True):
                try:
                    child.kill()
                except psutil.NoSuchProcess:
                    pass
        except psutil.NoSuchProcess:
            pass
        process.kill()
    process.wait(timeout=5)


OUTPUT_LIMIT = 16_000


@dataclass(frozen=True)
class ActionResult:
    outcome: str
    exit_code: int | None = None
    output: str | None = None


def _collect(stream, chunks: list[bytes]) -> None:
    """Keep the first and last parts of combined output and drain the rest."""
    head = tail = b""
    dropped = 0
    half = OUTPUT_LIMIT // 2
    while block := stream.read(8192):
        if len(head) < half:
            take = half - len(head)
            head, block = head + block[:take], block[take:]
        tail += block
        if len(tail) > half:
            dropped += len(tail) - half
            tail = tail[-half:]
    chunks.extend([head, tail, str(dropped).encode()])


def _decode(chunks: list[bytes]) -> str:
    if not chunks:
        return ""
    head, tail, dropped = chunks
    text = head.decode("utf-8", "replace")
    if int(dropped):
        text += f"\n[... {int(dropped)} bytes omitted ...]\n"
    return text + tail.decode("utf-8", "replace")


LIST_LIMIT = 500


def _read(path: Path) -> str:
    if path.is_dir():
        entries = sorted(path.iterdir(), key=lambda item: (not item.is_dir(), item.name.lower()))
        lines = [item.name + ("/" if item.is_dir() else f"  ({item.stat().st_size} bytes)")
                 for item in entries[:LIST_LIMIT]]
        if len(entries) > LIST_LIMIT:
            lines.append(f"[... {len(entries) - LIST_LIMIT} more entries ...]")
        return "\n".join(lines) or "(empty folder)"
    with path.open("rb") as handle:
        data = handle.read(OUTPUT_LIMIT + 1)
    if b"\0" in data[:4096]:
        return f"(binary file, {path.stat().st_size} bytes)"
    text = data[:OUTPUT_LIMIT].decode("utf-8", "replace")
    return text + (f"\n[... truncated at {OUTPUT_LIMIT} bytes ...]" if len(data) > OUTPUT_LIMIT else "")


def execute(action: PreparedAction, stop: Event) -> ActionResult:
    """Dispatch an approved action. Command output goes only to the caller, not to audit records."""
    for identity in action.paths:
        identity.verify()
    if stop.is_set():
        return ActionResult("cancelled")
    if action.action == "read":
        return ActionResult("completed", output=_read(Path(action.target)))
    if action.action == "open":
        if os.name == "nt":
            os.startfile(action.target)
        else:
            import sys

            launcher = shutil.which("open" if sys.platform == "darwin" else "xdg-open")
            if launcher is None:
                return ActionResult("handler_unavailable")
            # The normalized target starts with an absolute path or HTTP(S).
            process = subprocess.Popen([launcher, action.target], stdin=subprocess.DEVNULL,
                                       stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            try:
                if process.wait(timeout=0.5) != 0:
                    return ActionResult("nonzero_exit")
            except subprocess.TimeoutExpired:
                Thread(target=process.wait, name="desktop-opener", daemon=True).start()
        return ActionResult("dispatched")

    process = subprocess.Popen([action.target, *action.arguments], cwd=action.directory, shell=False,
                               stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               start_new_session=os.name != "nt")
    chunks: list[bytes] = []
    reader = Thread(target=_collect, args=(process.stdout, chunks), name="command-output", daemon=True)
    reader.start()
    deadline = monotonic() + 30
    try:
        while (code := process.poll()) is None:
            if stop.wait(0.05):
                _kill_command(process)
                return ActionResult("cancelled")
            if monotonic() >= deadline:
                _kill_command(process)
                reader.join(timeout=2)
                return ActionResult("timed_out", output=_decode(chunks))
        reader.join(timeout=5)
        return ActionResult("completed" if code == 0 else "nonzero_exit", code, _decode(chunks))
    finally:
        reader.join(timeout=2)
        if not reader.is_alive():
            process.stdout.close()
