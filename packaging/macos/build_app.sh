#!/usr/bin/env bash
# Build a self-contained, ad-hoc-signed Magnite.app (an unsigned testing build; ADR 0005).
#
#   build -> install bare bundle -> macdeployqt -> offscreen plugin -> stage runtime
#   -> finalize (clean, thin to arm64, sign) -> verify
#
# Usage: MAGNITE_QT_ROOT=<Qt>/6.8.3/macos bash packaging/macos/build_app.sh [--fresh-runtime]
#   --fresh-runtime  rebuild the Python runtime from scratch instead of resuming it
# The bundle is rebuilt from scratch every run; the runtime in build/packaging/macos/runtime
# is reused (dependencies re-resolved, backend wheel refreshed) and cloned into the bundle.
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
: "${MAGNITE_QT_ROOT:?set MAGNITE_QT_ROOT to the pinned Qt 6.8.3 macos directory (adr/0006-qt-provenance.md)}"
build_dir="$repo_dir/build-macos"
pkg_dir="$repo_dir/build/packaging/macos"
stage="$pkg_dir/stage"
app="$stage/Magnite.app"
runtime_src="$pkg_dir/runtime"
runtime_args=()
while [[ $# -gt 0 ]]; do
    case "$1" in
        --fresh-runtime) runtime_args+=(--fresh) ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done
[[ "$(uname -s)-$(uname -m)" == "Darwin-arm64" ]] || { echo "build_app.sh: needs an Apple-silicon Mac" >&2; exit 2; }
[[ -x "$MAGNITE_QT_ROOT/bin/macdeployqt" ]] || { echo "build_app.sh: no macdeployqt in $MAGNITE_QT_ROOT/bin" >&2; exit 2; }
if [[ -n "$(git -C "$repo_dir" status --porcelain)" ]]; then
    echo "warning: uncommitted changes; build-info/build.json will record working_tree_modified=true" >&2
fi

[[ -f "$pkg_dir/magnite.icns" ]] || bash "$repo_dir/packaging/macos/make_icon.sh"
cmake -S "$repo_dir/app" -B "$build_dir" -DCMAKE_PREFIX_PATH="$MAGNITE_QT_ROOT" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
    -DMAGNITE_MACOS_BUNDLE=ON -DOPENSSL_USE_STATIC_LIBS=TRUE
cmake --build "$build_dir" -j "${MAGNITE_BUILD_JOBS:-4}"
# install copies the whole build-tree bundle; drop any runtime test fixture left there (Phase 4).
rm -rf "$build_dir/Magnite.app/Contents/Resources/runtime"
rm -rf "$app"
cmake --install "$build_dir" --prefix "$stage"

# Qt first, on the bare bundle, so macdeployqt never walks the Python runtime (ADR 0007).
"$MAGNITE_QT_ROOT/bin/macdeployqt" "$app" -verbose=1
# macdeployqt deploys only the cocoa platform; --selftest and headless runs need offscreen
# (the Linux recipe adds it the same way).
cp "$MAGNITE_QT_ROOT/plugins/platforms/libqoffscreen.dylib" "$app/Contents/PlugIns/platforms/"

bash "$repo_dir/packaging/macos/prepare_runtime.sh" --destination "$runtime_src" ${runtime_args[@]+"${runtime_args[@]}"}
for item in runtime build-info licenses TESTING.md; do
    if [[ -e "$runtime_src/$item" ]]; then
        cp -c -R "$runtime_src/$item" "$app/Contents/Resources/$item"   # APFS clone
    fi
done

# Qt license texts and third-party notices (ISSUE-009); regenerate with packaging/macos/qt_notices.py.
mkdir -p "$app/Contents/Resources/licenses"
ditto "$repo_dir/packaging/macos/licenses/qt" "$app/Contents/Resources/licenses/qt"
if [[ -f "${OPENSSL_ROOT_DIR:-}/share/magnite-openssl/LICENSE.txt" ]]; then
    mkdir -p "$app/Contents/Resources/licenses/openssl"
    cp "$OPENSSL_ROOT_DIR/share/magnite-openssl/LICENSE.txt" "$app/Contents/Resources/licenses/openssl/"
fi

python3 "$repo_dir/packaging/macos/finalize_bundle.py" "$app"
python3 "$repo_dir/packaging/macos/verify_bundle.py" "$app"
du -sh "$app"
printf 'Bundle: %s\n' "$app"
