"""Refuse a candidate whose UPDATE_SEQUENCE does not exceed every published one.

Installed clients only move forward in sequence, so a reused or lower value would
never be offered to anyone. Reads a GitHub releases listing (as written by
`gh api --paginate --slurp`) and each published release's signed manifests.
"""
from __future__ import annotations

import argparse
import json
from pathlib import Path
import re
import runpy
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = re.compile(r"^update-[a-z]+-[a-z0-9_]+\.json$")


def releases(listing) -> list[dict]:
    """Flatten one page or a --slurp list of pages."""
    if listing and all(isinstance(page, list) for page in listing):
        return [release for page in listing for release in page]
    return list(listing)


def published_sequences(listing, fetch) -> dict[str, int]:
    found = {}
    for release in releases(listing):
        if release.get("draft"):
            continue
        for asset in release.get("assets", []):
            if MANIFEST.match(asset.get("name", "")):
                manifest = json.loads(fetch(asset["browser_download_url"]))
                found[release["tag_name"]] = max(found.get(release["tag_name"], 0), int(manifest["sequence"]))
    return found


def fetch(url: str) -> bytes:
    with urllib.request.urlopen(url, timeout=30) as response:  # public release assets over HTTPS
        return response.read(16384)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("listing", type=Path)
    args = parser.parse_args()
    candidate = runpy.run_path(str(ROOT / "backend/src/backend/_version.py"))
    sequences = published_sequences(json.loads(args.listing.read_text()), fetch)
    highest = max(sequences.values(), default=0)
    print(f"Published update sequences: {sequences or 'none'}")
    if candidate["UPDATE_SEQUENCE"] <= highest:
        print(f"::error::UPDATE_SEQUENCE {candidate['UPDATE_SEQUENCE']} must be greater than {highest}, "
              "the highest published sequence.")
        sys.exit(1)
    print(f"OK: {candidate['RELEASE_VERSION']} uses sequence {candidate['UPDATE_SEQUENCE']} (> {highest})")


if __name__ == "__main__":
    main()
