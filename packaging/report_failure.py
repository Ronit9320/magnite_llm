"""Expose bounded native failure details as public Actions annotations."""
from pathlib import Path
import sys


def escaped(value):
    return value.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


for root in sys.argv[1:]:
    logs = sorted(Path(root).rglob("*.log"), key=lambda p: p.stat().st_mtime, reverse=True)[:8]
    for log in logs:
        tail = "\n".join(log.read_text(errors="replace").splitlines()[-35:])[-10000:]
        if tail:
            print(f"::error title={escaped(log.name)}::{escaped(tail)}")
