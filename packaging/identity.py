"""Derived native package fields; the backend identity remains canonical."""
import json
from pathlib import Path
import re
import runpy


def identity():
    values = runpy.run_path(str(Path(__file__).resolve().parents[1] / "backend/src/backend/_version.py"))
    version = values["RELEASE_VERSION"]
    parts = re.match(r"v(\d+)\.(\d+)(?:\.(\d+))?", version)
    numeric = ".".join(part or "0" for part in parts.groups())
    return {"version": version, "sequence": values["UPDATE_SEQUENCE"],
            "data_schema": values["DATA_SCHEMA"], "numeric_version": numeric,
            "windows_version": f"{numeric}.{values['UPDATE_SEQUENCE']}"}


if __name__ == "__main__":
    print(json.dumps(identity()))
