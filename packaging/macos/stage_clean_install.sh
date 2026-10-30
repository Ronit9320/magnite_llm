#!/usr/bin/env bash
# Prepare the Phase 10 clean-install kit for a second macOS account.
#
# Usage: bash packaging/macos/stage_clean_install.sh [kit-dir]   (default /Users/Shared/magnite-clean-install)
# Copies the checksummed dmg, marks the COPY as downloaded by Safari (com.apple.quarantine), and adds the tester
# script, smoke_bundle.sh, the collect.sh helper and a RESULTS.md form. dist/ is never modified. Refuses to overwrite filled-in results.
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
kit="${1:-/Users/Shared/magnite-clean-install}"
version="$(python3 "$repo_dir/packaging/release_metadata.py" --print-version)"
dmg="$repo_dir/dist/Magnite-$version-arm64.dmg"

(cd "$repo_dir/dist" && shasum -a 256 -c SHA256SUMS-macos)
if [[ -f "$kit/RESULTS.md" ]] && ! cmp -s "$kit/RESULTS.md" <(sed -n '/^# Phase 10 results/,$p' "$repo_dir/packaging/macos/clean_install_results.md"); then
    echo "stage_clean_install.sh: $kit/RESULTS.md has been edited; move it away first" >&2; exit 1
fi
if [[ -e /Applications/Magnite.app ]]; then
    echo "warning: /Applications/Magnite.app exists; remove it so the second account does a fresh install" >&2
fi
mkdir -p "$kit"
kit="$(cd -- "$kit" && pwd -P)"
cp "$dmg" "$kit/Magnite-download.dmg"
xattr -w com.apple.quarantine "0181;$(printf %x "$(date +%s)");Safari;" "$kit/Magnite-download.dmg"
cp "$repo_dir/packaging/macos/smoke_bundle.sh" "$repo_dir/docs/release/macos/CLEAN_INSTALL.md" "$kit/"
cp "$repo_dir/packaging/macos/snapshot_bundle.py" "$kit/"
python3 - "$kit/CLEAN_INSTALL.md" "$version" "$kit" <<'PY'
from pathlib import Path
import sys
guide = Path(sys.argv[1])
guide.write_text(guide.read_text().replace("0.1.0-test.1", sys.argv[2])
                 .replace("/Users/Shared/magnite-clean-install", sys.argv[3]))
PY
cp "$repo_dir/packaging/macos/clean_install_results.md" "$kit/RESULTS.md"
cp "$repo_dir/packaging/macos/clean_install_collect.sh" "$kit/collect.sh"
chmod 0777 "$kit"; chmod 0644 "$kit/Magnite-download.dmg" "$kit/CLEAN_INSTALL.md" "$kit/snapshot_bundle.py"; chmod 0755 "$kit/smoke_bundle.sh" "$kit/collect.sh"; chmod 0666 "$kit/RESULTS.md"
echo "kit ready in $kit:"; ls -l "$kit"
echo "quarantine: $(xattr -p com.apple.quarantine "$kit/Magnite-download.dmg")"
