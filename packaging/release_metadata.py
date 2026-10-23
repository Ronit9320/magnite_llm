"""Validate the canonical identity and make an Ed25519-signed update manifest."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import runpy
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def identity():
    return runpy.run_path(str(ROOT / "backend/src/backend/_version.py"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--print-version", action="store_true")
    parser.add_argument("--appimage", type=Path)
    parser.add_argument("--repository", default=os.getenv("GITHUB_REPOSITORY", "Ronit9320/magnite_llm"))
    parser.add_argument("--key", type=Path)
    args = parser.parse_args()
    release = identity()
    version = release["RELEASE_VERSION"]
    if args.print_version:
        print(version)
        return
    if not args.appimage or not args.key:
        parser.error("--appimage and --key are required")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repository):
        parser.error("Invalid GitHub repository")
    image = args.appimage.resolve()
    expected = f"Magnite-{version}-linux-x86_64.AppImage"
    if image.name != expected:
        parser.error(f"Expected {expected}")
    with image.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    compatibility = json.loads((image.parent / "linux-compatibility.json").read_text())
    if compatibility["offenders"]:
        parser.error("The package failed the glibc compatibility audit")
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    manifest = {
        "format": 1, "version": version, "sequence": release["UPDATE_SEQUENCE"],
        "data_schema": release["DATA_SCHEMA"], "channel": "preview",
        "platform": "linux", "architecture": "x86_64", "variant": "cpu",
        "commit": commit, "size": image.stat().st_size, "sha256": digest,
        "url": f"https://github.com/{args.repository}/releases/download/{version}/{image.name}",
        "notes_url": f"https://github.com/{args.repository}/releases/tag/{version}",
        "minimum_glibc": compatibility["minimum_glibc"],
    }
    destination = image.parent / "update-linux-x86_64.json"
    destination.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    signature = destination.with_suffix(".json.sig")
    subprocess.run(["openssl", "pkeyutl", "-sign", "-rawin", "-inkey", str(args.key),
                    "-in", str(destination), "-out", str(signature)], check=True)
    # Refuse publication with a private key that doesn't match the shipped key.
    subprocess.run(["openssl", "pkeyutl", "-verify", "-rawin", "-pubin",
                    "-inkey", str(ROOT / "packaging/keys/update-public.pem"),
                    "-in", str(destination), "-sigfile", str(signature)], check=True)
    (image.parent / "SHA256SUMS").write_text(f"{digest}  {image.name}\n", encoding="utf-8")
    print(f"Signed {version} ({commit})")


if __name__ == "__main__":
    main()
