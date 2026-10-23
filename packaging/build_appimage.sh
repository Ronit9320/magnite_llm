#!/usr/bin/env bash
# Run from any directory. Requires CMake, Qt6, uv and network for build tools.
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
package_dir="$repo_dir/build/packaging"
app_dir="$package_dir/Magnite.AppDir"
tool_dir="$package_dir/tools"
mkdir -p "$tool_dir" "$repo_dir/dist"
release_version="$(python3 "$repo_dir/packaging/release_metadata.py" --print-version)"

cmake -S "$repo_dir/app" -B "$repo_dir/build" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMAGNITE_RELEASE_REPOSITORY="${GITHUB_REPOSITORY:-Ronit9320/magnite_llm}"
cmake --build "$repo_dir/build" -j "${MAGNITE_BUILD_JOBS:-2}"
python3 "$repo_dir/packaging/reset_native_stage.py"
cmake --install "$repo_dir/build" --prefix "$app_dir/usr"
cp "$repo_dir/packaging/AppRun" "$app_dir/AppRun"
chmod +x "$app_dir/AppRun"

runtime_args=()
if [[ -d "$app_dir/usr/bin/runtime/python" ]]; then runtime_args+=(--resume); fi
# Refresh the backend wheel and requirements on incremental builds too.
python3 "$repo_dir/packaging/prepare_runtime.py" \
    --destination "$app_dir/usr/bin" "${runtime_args[@]}"

python3 "$repo_dir/packaging/fetch_tools.py"

# Keep Qt's supplied notices with the test package when building on Arch.
for module in qt6-base qt6-svg qt6-wayland; do
    if [[ -d "/usr/share/licenses/$module" ]]; then
        mkdir -p "$app_dir/usr/bin/licenses/$module"
        cp -a "/usr/share/licenses/$module/." "$app_dir/usr/bin/licenses/$module/"
    fi
done
# Debian/Ubuntu Qt copyright metadata, alongside the Arch notices above.
for module in libqt6core6 libqt6gui6 libqt6widgets6 libqt6network6 libqt6svg6 libssl3 libstdc++6 libgcc-s1 gcc-11-base; do
    if [[ -f "/usr/share/doc/$module/copyright" ]]; then
        mkdir -p "$app_dir/usr/bin/licenses/$module"
        cp "/usr/share/doc/$module/copyright" "$app_dir/usr/bin/licenses/$module/"
    fi
done
for module in openssl gcc gcc-libs; do
    if [[ -d "/usr/share/licenses/$module" ]]; then
        mkdir -p "$app_dir/usr/bin/licenses/$module"
        cp -a "/usr/share/licenses/$module/." "$app_dir/usr/bin/licenses/$module/"
    fi
done
export PATH="$tool_dir:$PATH"
export APPIMAGE_EXTRACT_AND_RUN=1
export QMAKE="${QMAKE:-$(command -v qmake6)}"
export EXTRA_PLATFORM_PLUGINS=libqoffscreen.so
qt_plugins="$($QMAKE -query QT_INSTALL_PLUGINS)"
for plugin in "$qt_plugins"/platforms/libqwayland*.so; do
    if [[ -f "$plugin" ]]; then EXTRA_PLATFORM_PLUGINS+=";$(basename "$plugin")"; fi
done
# linuxdeploy's older bundled strip cannot read modern ELF RELR sections.
export NO_STRIP=1
export LDAI_OUTPUT="$repo_dir/dist/Magnite-$release_version-linux-x86_64.AppImage"
export LINUXDEPLOY_OUTPUT_VERSION="$release_version"
export LDAI_RUNTIME_FILE="$tool_dir/runtime-x86_64"
"$tool_dir/linuxdeploy-x86_64.AppImage" \
    --appdir "$app_dir" \
    --executable "$repo_dir/build/magnite-app" \
    --library "$(c++ -print-file-name=libstdc++.so.6)" \
    --library "$(cc -print-file-name=libgcc_s.so.1)" \
    --desktop-file "$repo_dir/packaging/magnite.desktop" \
    --icon-file "$repo_dir/app/resources/brand/symbol.svg" \
    --icon-filename magnite --plugin qt --output appimage

python3 "$repo_dir/packaging/audit_glibc.py" "$app_dir" \
    --maximum "${MAGNITE_GLIBC_MAX:-2.35}" --output "$repo_dir/dist/linux-compatibility.json"

cd "$repo_dir/dist"
sha256sum "Magnite-$release_version-linux-x86_64.AppImage" > SHA256SUMS
cp "$repo_dir/packaging/TESTING.md" TESTING.md
printf 'Test AppImage: %s\n' "$LDAI_OUTPUT"
