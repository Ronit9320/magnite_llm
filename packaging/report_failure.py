"""Expose bounded native failure details as public Actions annotations."""
from pathlib import Path
import sys


def escaped(value):
    return value.replace("%", "%25").replace("\r", "%0D").replace("\n", "%0A")


for root in sys.argv[1:]:
    logs = sorted(Path(root).rglob("*.log"), key=lambda p: p.stat().st_mtime, reverse=True)[:8]
    for log in logs:
        lines = log.read_text(errors="replace").splitlines()
        # UI checks continue after assertions fail; retain those failures even
        # when successful checks push them out of the final log lines.
        failures = [line for line in lines[:-35] if "FAIL:" in line or "FAIL (" in line]
        tail = "\n".join(failures[-25:] + lines[-35:])[-10000:]
        if tail:
            print(f"::error title={escaped(log.name)}::{escaped(tail)}")
