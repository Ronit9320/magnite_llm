"""Record bundle bytes and attributes to diagnose changes across installed runs."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat


def snapshot(root):
    result = {}
    for path in sorted(root.rglob("*")):
        attributes = path.lstat()
        record = {"mode": stat.S_IMODE(attributes.st_mode)}
        if path.is_symlink():
            record["link"] = os.readlink(path)
        elif path.is_file():
            with path.open("rb") as stream:
                record["sha256"] = hashlib.file_digest(stream, "sha256").hexdigest()
        if hasattr(os, "listxattr"):
            record["xattrs"] = {name: hashlib.sha256(os.getxattr(path, name, follow_symlinks=False)).hexdigest()
                                for name in os.listxattr(path, follow_symlinks=False)}
        result[path.relative_to(root).as_posix()] = record
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("app", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--compare", type=Path)
    args = parser.parse_args()
    current = snapshot(args.app)
    args.output.write_text(json.dumps(current, indent=2) + "\n")
    if args.compare:
        before = json.loads(args.compare.read_text())
        changed = False
        for path in sorted(set(current) | set(before)):
            if before.get(path) != current.get(path):
                print(path, "BEFORE", before.get(path), "AFTER", current.get(path))
                # macOS may attach provenance/quarantine metadata when an app
                # first runs. Record it, while code signing independently
                # checks seal-relevant attributes. File mutations always fail.
                old = {k: v for k, v in (before.get(path) or {}).items() if k != "xattrs"}
                new = {k: v for k, v in (current.get(path) or {}).items() if k != "xattrs"}
                changed |= old != new
        raise SystemExit(1 if changed else 0)
