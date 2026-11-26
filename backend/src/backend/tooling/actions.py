"""Validate an exact local action and dispatch it after the broker claims approval."""

from __future__ import annotations

from dataclasses import dataclass
import os
from pathlib import Path
import shutil
import signal
import stat
import subprocess
import tempfile
from threading import Event, Thread
from time import monotonic
from typing import Annotated, Literal
from urllib.parse import urlsplit

from pydantic import AfterValidator, BaseModel, ConfigDict, Field, TypeAdapter

from backend.paths import app_home
from backend.textcheck import ensure_utf8


CONTENT_LIMIT = 16_000
# Characters that reorder text on screen, so a preview could read differently from the file.
_DIRECTION_OVERRIDES = {chr(code) for code in (*range(0x202A, 0x202F), *range(0x2066, 0x206A))}


class Rejected(ValueError):
    """An action that cannot be offered for approval, with a short reason code."""

    def __init__(self, reason: str):
        super().__init__(reason)
        self.reason = reason


def _argument(value: str) -> str:
    ensure_utf8(value, "Tool argument")
    if any(ord(ch) < 32 or ord(ch) == 127 for ch in value):
        raise ValueError("Tool arguments must be single-line text")
    if any(ch in _DIRECTION_OVERRIDES for ch in value):
        raise ValueError("Tool arguments may not contain text direction characters")
    return value


def _content(value: str) -> str:
    ensure_utf8(value, "File content")
    if len(value.encode("utf-8")) > CONTENT_LIMIT:
        raise ValueError(f"File content is limited to {CONTENT_LIMIT} bytes")
    if any(ch in _DIRECTION_OVERRIDES or (ord(ch) < 32 and ch not in "\n\r\t") or 127 <= ord(ch) <= 159
           for ch in value):
        raise ValueError("File content may not contain control or text direction characters")
    return value


Argument = Annotated[str, Field(max_length=4096), AfterValidator(_argument)]
Target = Annotated[Argument, Field(min_length=1)]
Content = Annotated[str, AfterValidator(_content)]


class OpenAction(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, frozen=True)
    action: Literal["open"]
    target: Target


class ReadAction(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, frozen=True)
    action: Literal["read"]
    target: Target


class WriteAction(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, frozen=True)
    action: Literal["write"]
    target: Target
    content: Content


class CommandAction(BaseModel):
    model_config = ConfigDict(extra="forbid", strict=True, frozen=True)
    action: Literal["command"]
    executable: Target
    arguments: list[Argument] = Field(max_length=32)


PROPOSAL = TypeAdapter(Annotated[OpenAction | ReadAction | WriteAction | CommandAction,
                                 Field(discriminator="action")])


_MODEL_KINDS = {"run_command": "command", "read_file": "read", "list_folder": "read",
                "open_target": "open", "write_file": "write"}


def describe(request) -> tuple[str, dict]:
    """Summarize a refused request for its card: a broker action or a model tool call.

    File content is never included.
    """
    def text(value) -> str:
        return str("" if value is None else value)[:4096].encode("utf-8", "replace").decode("utf-8")

    request = request if isinstance(request, dict) else {}
    if "action" in request:
        kind = request["action"] if request["action"] in ("read", "open", "command", "write") else "unknown"
        if kind == "command":
            arguments = request.get("arguments")
            return kind, {"action": kind, "executable": text(request.get("executable")),
                          "arguments": [text(item) for item in arguments[:32]]
                          if isinstance(arguments, list) else []}
        return kind, {"action": kind, "target": text(request.get("target"))}
    name = request.get("name")
    arguments = request.get("arguments")
    arguments = arguments if isinstance(arguments, dict) else {}
    kind = _MODEL_KINDS.get(name, "unknown") if isinstance(name, str) else "unknown"
    target = {"command": arguments.get("command"), "read": arguments.get("path") or ".",
              "open": arguments.get("target"), "write": arguments.get("path"), "unknown": name}[kind]
    return kind, {"action": kind, "target": text(target)}


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
    action: Literal["open", "read", "command", "write"]
    target: str
    arguments: tuple[str, ...]
    directory: Path
    paths: tuple[PathIdentity, ...]
    content: str = ""
    existing_bytes: int | None = None  # Set when a write replaces a file.

    def display(self) -> dict:
        if self.action == "write":
            shown = {"action": "write", "target": self.target, "content": self.content,
                     "replaces": self.existing_bytes is not None}
            if self.existing_bytes is not None:
                shown["existing_bytes"] = self.existing_bytes
            return shown
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


def _existing_target(value: str, directory: Path) -> Path:
    try:
        return _resolve_path(value, directory)
    except (FileNotFoundError, NotADirectoryError) as exc:
        raise Rejected("not_found") from exc


def _same(first: Path, second: Path) -> bool:
    try:
        return os.path.samefile(first, second)
    except OSError:
        return False


def _protected(path: Path) -> bool:
    """Magnite's own data, apart from the tool workspace. Compared by identity, not spelling."""
    home = Path(app_home()).expanduser()
    for parent in path.parents:
        if _same(parent, home / "tool-workspace"):
            return False
        if _same(parent, home):
            return True
    return False


def _prepare_write(action: WriteAction, directory: Path) -> PreparedAction:
    path = (directory / Path(action.target).expanduser()).resolve()
    if not path.parent.is_dir():
        raise Rejected("folder_not_found")
    if path.is_symlink() or (path.exists() and not path.is_file()):
        raise Rejected("not_a_file")
    if _protected(path):
        raise Rejected("protected_location")
    if path.exists():
        return PreparedAction("write", str(path), (), directory, (PathIdentity.read(path),),
                              action.content, path.stat().st_size)
    return PreparedAction("write", str(path), (), directory, (PathIdentity.read(path.parent),), action.content)


def prepare(proposal, directory: Path) -> PreparedAction:
    """Validate a proposal and pin the paths it names. Every refusal is a ``Rejected``."""
    try:
        return _prepare(PROPOSAL.validate_python(proposal), directory)
    except Rejected:
        raise
    except (ValueError, OSError, RuntimeError) as exc:
        raise Rejected("invalid_request") from exc


def _prepare(action, directory: Path) -> PreparedAction:
    if isinstance(action, WriteAction):
        return _prepare_write(action, directory)
    if isinstance(action, ReadAction):
        path = _existing_target(action.target, directory)
        if not (path.is_file() or path.is_dir()):
            raise Rejected("not_a_file")
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
        path = _existing_target(value, directory)
        if not (path.is_file() or path.is_dir()):
            raise Rejected("not_a_file")
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


def _write(action: PreparedAction) -> ActionResult:
    path = Path(action.target)
    data = action.content.encode("utf-8")
    if action.existing_bytes is None:
        # Exclusive: a file that appeared after approval is never overwritten.
        with path.open("xb") as handle:
            try:
                handle.write(data)
                handle.flush()
            except OSError:
                path.unlink(missing_ok=True)
                raise
    else:
        mode = stat.S_IMODE(path.stat().st_mode)
        descriptor, temporary = tempfile.mkstemp(prefix=".magnite-", dir=path.parent)
        try:
            with os.fdopen(descriptor, "wb") as handle:
                handle.write(data)
            os.chmod(temporary, mode)
            action.paths[0].verify()
            os.replace(temporary, path)
        finally:
            Path(temporary).unlink(missing_ok=True)
    return ActionResult("completed", output=f"Wrote {len(data)} bytes to {path}.")


def execute(action: PreparedAction, stop: Event) -> ActionResult:
    """Dispatch an approved action. Command output goes only to the caller, not to audit records."""
    for identity in action.paths:
        identity.verify()
    if stop.is_set():
        return ActionResult("cancelled")
    if action.action == "write":
        return _write(action)
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
