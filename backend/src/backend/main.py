import argparse
import ctypes
import os
import signal
import socket
import sys
import threading
import time

import uvicorn

from backend import local_access
from backend.paths import app_home
from backend.server import app

_PDEATHSIG_SIGTERM = 15
_PARENT_POLL_SECONDS = 1.0
_SHUTDOWN_GRACE_SECONDS = 15.0


def _watch_parent(parent: int, interval: float | None = None) -> None:
    """macOS has no PR_SET_PDEATHSIG: poll until we are reparented, then take
    uvicorn's graceful SIGTERM path, and exit hard if shutdown stalls."""
    interval = _PARENT_POLL_SECONDS if interval is None else interval
    while os.getppid() == parent:
        time.sleep(interval)
    os.kill(os.getpid(), signal.SIGTERM)
    time.sleep(_SHUTDOWN_GRACE_SECONDS)
    os._exit(1)


def _enable_parent_death_signal() -> None:
    """Ask the kernel to SIGTERM us if our parent process dies, so a crashed
    or force-killed GUI cannot leave an orphaned backend behind."""
    if sys.platform == "linux" and os.name == "posix":
        try:
            PR_SET_PDEATHSIG = 1
            libc = ctypes.CDLL("libc.so.6", use_errno=True)
            if libc.prctl(PR_SET_PDEATHSIG, _PDEATHSIG_SIGTERM) != 0:
                raise OSError(ctypes.get_errno(), "prctl failed")
        except (OSError, AttributeError):
            pass
    elif sys.platform == "darwin":
        parent = os.getppid()
        if parent != 1:   # launched by launchd itself: there is no parent to outlive
            threading.Thread(target=_watch_parent, args=(parent,), name="parent-watch",
                             daemon=True).start()


def find_free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def main():
    parser = argparse.ArgumentParser(description="Magnite LLM Backend")
    parser.add_argument("--port", type=int, default=0, help="Port to listen on (0 = random)")
    parser.add_argument(
        "--home",
        default=None,
        help="Data directory for the registry (default: ~/.magnite)",
    )
    args = parser.parse_args()

    if args.home:
        os.environ["MAGNITE_HOME"] = os.path.abspath(args.home)

    _enable_parent_death_signal()

    token = local_access.take_token()
    try:
        data_lock = local_access.acquire_data_dir_lock(app_home())  # noqa: F841 -- held until exit
    except local_access.DataDirInUse as exc:
        print(f"[magnite] {exc}", file=sys.stderr)
        sys.exit(local_access.EXIT_DATA_DIR_IN_USE)

    port = args.port if args.port else find_free_port()
    print(f"Magnite backend starting on http://127.0.0.1:{port}", file=sys.stderr)
    uvicorn.run(local_access.LocalAccessMiddleware(app, token), host="127.0.0.1", port=port, log_level="info")


if __name__ == "__main__":
    main()