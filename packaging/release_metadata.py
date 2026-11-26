"""Validate the canonical identity and make Ed25519-signed update manifests.

One manifest per platform, named update-<platform>-<architecture>.json, sits
next to the package it describes. Installed clients only trust a manifest whose
detached signature verifies against packaging/keys/update-public.pem.
"""
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
# Platform -> (architecture, package filename). Keep in step with UpdateManager::packageName.
PLATFORMS = {
    "linux": ("x86_64", "Magnite-{version}-linux-x86_64.AppImage"),
    "macos": ("arm64", "Magnite-{version}-macos-arm64.zip"),
    "windows": ("x86_64", "Magnite-{version}-windows-x86_64-setup.exe"),
}
# Matches LSMinimumSystemVersion (app/CMakeLists.txt) for the published arm64 bundle.
MINIMUM_MACOS = "14.0"


def identity():
    return runpy.run_path(str(ROOT / "backend/src/backend/_version.py"))


def minimum_windows() -> str:
    """The installer's MinVersion, so the updater and installer agree."""
    text = (ROOT / "packaging/windows/magnite.iss").read_text(encoding="utf-8")
    match = re.search(r"(?m)^MinVersion=(10\.0\.\d+)\s*$", text)
    if not match:
        raise SystemExit("packaging/windows/magnite.iss must set MinVersion=10.0.<build>")
    return match[1]


def compatibility(platform: str, package: Path) -> dict:
    if platform == "linux":
        report = json.loads((package.parent / "linux-compatibility.json").read_text())
        if report["offenders"]:
            raise SystemExit("The package failed the glibc compatibility audit")
        return {"minimum_glibc": report["minimum_glibc"]}
    if platform == "macos":
        return {"minimum_macos": MINIMUM_MACOS}
    return {"minimum_windows": minimum_windows()}


def sign(manifest: Path, key: Path) -> Path:
    signature = manifest.with_suffix(".json.sig")
    subprocess.run(["openssl", "pkeyutl", "-sign", "-rawin", "-inkey", str(key),
                    "-in", str(manifest), "-out", str(signature)], check=True)
    # Refuse publication with a private key that doesn't match the shipped key.
    subprocess.run(["openssl", "pkeyutl", "-verify", "-rawin", "-pubin",
                    "-inkey", str(ROOT / "packaging/keys/update-public.pem"),
                    "-in", str(manifest), "-sigfile", str(signature)], check=True)
    return signature


def make_manifest(platform: str, package: Path, repository: str, release: dict, commit: str) -> Path:
    architecture, pattern = PLATFORMS[platform]
    version = release["RELEASE_VERSION"]
    expected = pattern.format(version=version)
    if package.name != expected:
        raise SystemExit(f"Expected {expected}, got {package.name}")
    with package.open("rb") as stream:
        digest = hashlib.file_digest(stream, "sha256").hexdigest()
    manifest = {
        "format": 1, "version": version, "sequence": release["UPDATE_SEQUENCE"],
        "data_schema": release["DATA_SCHEMA"], "channel": "preview",
        "platform": platform, "architecture": architecture, "variant": "cpu",
        "commit": commit, "size": package.stat().st_size, "sha256": digest,
        "url": f"https://github.com/{repository}/releases/download/{version}/{package.name}",
        "notes_url": f"https://github.com/{repository}/releases/tag/{version}",
        **compatibility(platform, package),
    }
    destination = package.parent / f"update-{platform}-{architecture}.json"
    destination.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    return destination


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--print-version", action="store_true")
    parser.add_argument("--platform", choices=sorted(PLATFORMS), default="linux")
    parser.add_argument("--package", type=Path, help="The exact tested package to describe")
    parser.add_argument("--appimage", type=Path, help="Legacy spelling of --platform linux --package")
    parser.add_argument("--repository", default=os.getenv("GITHUB_REPOSITORY", "Ronit9320/magnite_llm"))
    parser.add_argument("--commit", help="Source commit (default: HEAD of this checkout)")
    parser.add_argument("--key", type=Path)
    args = parser.parse_args()
    release = identity()
    if args.print_version:
        print(release["RELEASE_VERSION"])
        return
    package = args.package or args.appimage
    if args.appimage and (args.package or args.platform != "linux"):
        parser.error("--appimage cannot be combined with --package or another platform")
    if not package or not args.key:
        parser.error("--package (or --appimage) and --key are required")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", args.repository):
        parser.error("Invalid GitHub repository")
    commit = args.commit or subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
    if not re.fullmatch(r"[0-9a-f]{40}", commit):
        parser.error("Invalid source commit")
    package = package.resolve()
    manifest = make_manifest(args.platform, package, args.repository, release, commit)
    sign(manifest, args.key)
    if args.appimage:
        # The legacy Linux-only publisher also expects this checksum file.
        digest = json.loads(manifest.read_text())["sha256"]
        (package.parent / "SHA256SUMS").write_text(f"{digest}  {package.name}\n", encoding="utf-8")
    print(f"Signed {manifest.name} for {release['RELEASE_VERSION']} ({commit})")


if __name__ == "__main__":
    main()
