"""Exercise the shipped Pi worker with an offline provider and bundled Node."""
import os
from pathlib import Path
import sys
from threading import Event
from unittest.mock import patch

from backend._features import BLUEPRINTS_AND_TOOLS
from backend.tooling import pi_bridge


def reply(messages, tools, engine, stop, native):
    return '{"answer":"packaged Pi is ready"}'


def unexpected_action(proposal):
    raise RuntimeError("The packaging smoke must not request external actions")


def check_bridge():
    with patch.object(pi_bridge, "_generate", reply):
        result = pi_bridge.run("Reply with the packaging status.", None, unexpected_action, Event())
    assert result == "packaged Pi is ready", result


def main():
    os.environ["PATH"] = ""
    if not BLUEPRINTS_AND_TOOLS:
        from backend.server import app
        node = Path(sys.prefix).parent / "node"
        assert not node.exists(), f"{node} is bundled although Tool mode is switched off"
        paths = set(app.openapi()["paths"])
        assert "/inference/tool-runs" not in paths and "/blueprints" not in paths, "tool routes are mounted"
        print("Bundled Node and Pi bridge OK: switched off in this build, nothing bundled")
        return
    node = Path(pi_bridge.node_path())
    assert node.parent == Path(sys.prefix).parent / "node", node
    check_bridge()
    print("Bundled Node and Pi bridge OK")


if __name__ == "__main__":
    main()
