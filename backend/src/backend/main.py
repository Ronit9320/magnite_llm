import argparse
import os
import socket
import sys

import uvicorn

from backend.server import app


def _enable_parent_death_signal() -> None:
    """Ask the kernel to SIGTERM us if our parent process dies, so a crashed
    or force-killed GUI cannot leave an orphaned backend behind."""
    if sys.platform == "linux" and os.name == "posix":
        try:
            import ctypes

            _PDEATHSIG_SIGTERM = 15
            PR_SET_PDEATHSIG = 1
            libc = ctypes.CDLL("libc.so.6", use_errno=True)
            if libc.prctl(PR_SET_PDEATHSIG, _PDEATHSIG_SIGTERM) != 0:
                raise OSError(ctypes.get_errno(), "prctl failed")
        except (OSError, AttributeError):
            pass


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

    port = args.port if args.port else find_free_port()
    print(f"Magnite backend starting on http://127.0.0.1:{port}", file=sys.stderr)
    uvicorn.run(app, host="127.0.0.1", port=port, log_level="info")


if __name__ == "__main__":
    main()