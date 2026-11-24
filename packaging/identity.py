"""Derived native package fields; the backend identity remains canonical."""
import json
from pathlib import Path
import re
import runpy


SOURCE = Path(__file__).resolve().parents[1] / "backend/src/backend"


def features():
    """The build-time feature profile; see backend/src/backend/_features.py."""
    values = runpy.run_path(str(SOURCE / "_features.py"))
    return {"blueprints_and_tools": bool(values["BLUEPRINTS_AND_TOOLS"])}


def identity():
    values = runpy.run_path(str(SOURCE / "_version.py"))
    version = values["RELEASE_VERSION"]
    parts = re.match(r"v(\d+)\.(\d+)(?:\.(\d+))?", version)
    numeric = ".".join(part or "0" for part in parts.groups())
    return {"version": version, "sequence": values["UPDATE_SEQUENCE"],
            "data_schema": values["DATA_SCHEMA"], "numeric_version": numeric,
            "windows_version": f"{numeric}.{values['UPDATE_SEQUENCE']}",
            "features": features()}


if __name__ == "__main__":
    import sys
    if sys.argv[1:] == ["--blueprints-and-tools"]:
        print("1" if features()["blueprints_and_tools"] else "0")
    else:
        print(json.dumps(identity()))
