"""Fetch AppImage tools and reject changed upstream continuous-channel assets."""
import hashlib
import json
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[1]
destination = ROOT / "build/packaging/tools"
destination.mkdir(parents=True, exist_ok=True)
for entry in json.loads((ROOT / "packaging/tools.lock.json").read_text()):
    target = destination / entry["name"]
    if not target.exists():
        temporary = target.with_suffix(".download")
        subprocess.run(["curl", "--fail", "--location", "--retry", "3", "--proto", "=https",
                        entry["url"], "--output", str(temporary)], check=True)
        temporary.replace(target)
    with target.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    if digest != entry["sha256"]:
        raise SystemExit(f"Checksum mismatch for {target.name}. Review and refresh tools.lock.json; do not bypass verification.")
    target.chmod(0o755)
