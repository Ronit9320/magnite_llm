#!/usr/bin/env bash
# Wrap a finished Magnite.app in a compressed, checksummed testing .dmg (hdiutil only).
#
# Usage: bash packaging/macos/build_dmg.sh [path/to/Magnite.app]
#   default app: build/packaging/macos/stage/Magnite.app (from build_app.sh)
# Output: dist/Magnite-<label>-arm64.dmg and dist/SHA256SUMS-macos (the Linux build owns dist/SHA256SUMS).
# The volume holds Magnite.app, an Applications symlink to drag it onto, and TESTING.md once it exists.
# Refuses to package a bundle that fails verify_bundle.py. Safe to re-run (-ov overwrites).
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
app="$(cd -- "${1:-$repo_dir/build/packaging/macos/stage/Magnite.app}" && pwd)"
label="$(/usr/libexec/PlistBuddy -c 'Print :MagniteBuildLabel' "$app/Contents/Info.plist")"
name="Magnite-$label-arm64.dmg"
dist="$repo_dir/dist"
root="$repo_dir/build/packaging/macos/dmg-root"

python3 "$repo_dir/packaging/macos/verify_bundle.py" "$app"
build_json="$app/Contents/Resources/build-info/build.json"
if grep -q '"working_tree_modified": true' "$build_json"; then
    echo "warning: this bundle was built from a modified tree (see $build_json)" >&2
fi

rm -rf "$root"
mkdir -p "$root" "$dist"
ditto "$app" "$root/Magnite.app"
ln -s /Applications "$root/Applications"
testing="$repo_dir/packaging/macos/TESTING.md"
if [[ -f "$testing" ]]; then
    cp "$testing" "$root/TESTING.md"
else
    echo "note: packaging/macos/TESTING.md not written yet; the volume has no tester notes" >&2
fi

hdiutil create -volname "Magnite $label" -srcfolder "$root" -ov -format UDZO -fs HFS+ "$dist/$name"
hdiutil verify "$dist/$name"
rm -rf "$root"
(cd "$dist" && shasum -a 256 "$name" > SHA256SUMS-macos && cat SHA256SUMS-macos)
ls -lh "$dist/$name"
