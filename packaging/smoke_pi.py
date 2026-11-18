"""Exercise the shipped Pi worker with an offline provider and bundled Node."""
import os
from pathlib import Path
import sys
from threading import Event
from unittest.mock import patch

from backend.tooling import pi_bridge


async def reply(messages, engine, stop):
    return '{"answer":"packaged Pi is ready"}'


def unexpected_action(proposal):
    raise RuntimeError("The packaging smoke must not request external actions")


def main():
    os.environ["PATH"] = ""
    node = Path(pi_bridge.node_path())
    assert node.parent == Path(sys.prefix).parent / "node", node
    with patch.object(pi_bridge, "_local_reply", reply):
        result = pi_bridge.run("Reply with the packaging status.", None, unexpected_action, Event())
    assert result == "packaged Pi is ready", result
    print("Bundled Node and Pi bridge OK")


if __name__ == "__main__":
    main()
