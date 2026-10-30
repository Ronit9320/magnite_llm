#!/usr/bin/env bash
# Mount a testing .dmg read-only, check its layout, and smoke-test the app straight from the volume.
#
# Usage: bash packaging/macos/check_dmg.sh dist/Magnite-<label>-arm64.dmg [log-dir]
# The volume is read-only, so smoke_bundle.sh's "signature after run" also proves the app never
# writes into its own bundle. Always detaches. Exit 0 only if everything passed.
set -uo pipefail
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
dmg="$(cd -- "$(dirname -- "$1")" && pwd)/$(basename -- "$1")"
log_dir="${2:-${TMPDIR%/}/magnite-dmg-check}"
# Physical path: mount(8) reports /private/var/..., not the /var/... symlink TMPDIR uses (ISSUE-005).
mnt="$(cd -P -- "$(mktemp -d "${TMPDIR%/}/magnite-dmg.XXXXXX")" && pwd)"
failures=0
fail() { echo "FAIL  $*"; failures=$((failures + 1)); }
pass() { echo "PASS  $*"; }
detach() { hdiutil detach "$mnt" -quiet 2>/dev/null || hdiutil detach "$mnt" -force -quiet; rmdir "$mnt" 2>/dev/null; }

if hdiutil verify "$dmg" >/dev/null 2>&1; then pass "hdiutil verify"; else fail "hdiutil verify"; fi
if ! hdiutil attach "$dmg" -readonly -nobrowse -noautoopen -mountpoint "$mnt" >/dev/null; then
    fail "attach"; echo "$failures failure(s)"; exit 1
fi
trap detach EXIT

entries="$(ls -A "$mnt" | grep -v '^\.' | sort | tr '\n' ' ')"
case "$entries" in
    "Applications Magnite.app "|"Applications Magnite.app TESTING.md ") pass "volume contents: $entries" ;;
    *) fail "volume contents: '$entries'" ;;
esac
if [[ -L "$mnt/Applications" && "$(readlink "$mnt/Applications")" == "/Applications" ]]; then
    pass "Applications -> /Applications"
else
    fail "Applications link: $(readlink "$mnt/Applications" 2>/dev/null || echo missing)"
fi
if mount | grep -F " on $mnt " | grep -q "read-only"; then pass "mounted read-only"; else fail "volume is not read-only"; fi

bash "$repo_dir/packaging/macos/smoke_bundle.sh" "$mnt/Magnite.app" "$log_dir" || failures=$((failures + 1))

echo "$failures failure(s) for $(basename "$dmg")"
exit $((failures > 0))
