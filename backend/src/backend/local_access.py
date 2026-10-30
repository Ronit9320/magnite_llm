"""Keep the local API to the app that launched it (ISSUE-011, docs/release/macos/adr/0009).

127.0.0.1 is shared by every user account and reachable by web pages through DNS rebinding, so the
network-facing server checks two things on every request: the Host names a loopback address, and the
caller presents the per-launch secret the app passed in MAGNITE_API_TOKEN. A data-folder lock keeps two
backends from writing the same databases.
"""
from __future__ import annotations

import errno
import hmac
import os
import sys
import time

try:
    import fcntl
except ImportError:  # Windows uses a one-byte CRT lock.
    fcntl = None
    import msvcrt

TOKEN_ENV = "MAGNITE_API_TOKEN"
EXIT_DATA_DIR_IN_USE = 75          # EX_TEMPFAIL; the app reports "already running"
_LOOPBACK_HOSTS = {"127.0.0.1", "localhost", "[::1]"}
_OPEN_PATHS = {"/health"}          # readiness probe; says only "ok"


def _host_name(host: str) -> str:
    host = host.strip().lower()
    if host.startswith("["):
        return host[:host.find("]") + 1]
    return host.rsplit(":", 1)[0] if host.count(":") == 1 else host


class LocalAccessMiddleware:
    """Pure ASGI, so streaming responses and disconnect detection pass through untouched."""

    def __init__(self, app, token: str | None, shutdown=None):
        self.app = app
        self._shutdown = shutdown
        self._expected = f"Bearer {token}".encode() if token else None

    async def __call__(self, scope, receive, send):
        if scope["type"] != "http":
            return await self.app(scope, receive, send)
        headers = dict(scope.get("headers") or [])
        if _host_name(headers.get(b"host", b"").decode("latin-1")) not in _LOOPBACK_HOSTS:
            return await _reject(send, 400, b"Host not allowed")
        if self._expected is not None and scope.get("path") not in _OPEN_PATHS:
            if not hmac.compare_digest(headers.get(b"authorization", b""), self._expected):
                return await _reject(send, 401, b"Missing or invalid credentials")
        if scope.get("path") == "/_desktop/shutdown" and scope.get("method") == "POST":
            if self._expected is None or self._shutdown is None:
                return await _reject(send, 404, b"Not available")
            self._shutdown()
            return await _reject(send, 200, b"Shutting down")
        return await self.app(scope, receive, send)


async def _reject(send, status: int, detail: bytes):
    body = b'{"detail":"' + detail + b'"}'
    await send({"type": "http.response.start", "status": status,
                "headers": [(b"content-type", b"application/json"), (b"content-length", str(len(body)).encode())]})
    await send({"type": "http.response.body", "body": body})


def take_token() -> str | None:
    """Read the launch secret and drop it from the environment so no child process inherits it."""
    token = os.environ.pop(TOKEN_ENV, "") or None
    if token is None:
        print("[magnite] warning: MAGNITE_API_TOKEN not set; API open to local callers "
              "(developer mode; Host check still applies)", file=sys.stderr)
    return token


class DataDirInUse(RuntimeError):
    pass


def acquire_data_dir_lock(home: str, wait: float = 5.0):
    """Hold an exclusive lock on <home>/.backend.lock for the life of the process; the OS releases it on
    any exit, including SIGKILL. Waits briefly so a just-killed predecessor can finish exiting."""
    os.makedirs(home, exist_ok=True)
    handle = open(os.path.join(home, ".backend.lock"), "a+b")
    deadline = time.monotonic() + wait
    while True:
        try:
            if fcntl is not None:
                fcntl.flock(handle.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            else:
                handle.seek(0)
                msvcrt.locking(handle.fileno(), msvcrt.LK_NBLCK, 1)
            return handle
        except OSError as exc:
            if exc.errno not in (errno.EACCES, errno.EAGAIN, errno.EDEADLK):
                handle.close()
                raise
            if time.monotonic() >= deadline:
                handle.close()
                raise DataDirInUse(f"data folder {home} is in use by another Magnite backend") from None
            time.sleep(0.2)
