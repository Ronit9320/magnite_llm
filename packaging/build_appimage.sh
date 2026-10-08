#!/usr/bin/env bash
# Run from any directory. Requires CMake, Qt6, uv and network for build tools.
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
package_dir="$repo_dir/build/packaging"
app_dir="$package_dir/Magnite.AppDir"
tool_dir="$package_dir/tools"
mkdir -p "$tool_dir" "$repo_dir/dist"

cmake -S "$repo_dir/app" -B "$repo_dir/build" \
    -DCMAKE_BUILD_TYPE=Release -DMAGNITE_BUILD_LABEL=0.1.0-test.1
cmake --build "$repo_dir/build" -j "${MAGNITE_BUILD_JOBS:-2}"
cmake --install "$repo_dir/build" --prefix "$app_dir/usr"

runtime_args=()
if [[ -d "$app_dir/usr/bin/runtime/python" ]]; then runtime_args+=(--resume); fi
# Refresh the backend wheel and requirements on incremental builds too.
python3 "$repo_dir/packaging/prepare_runtime.py" \
    --destination "$app_dir/usr/bin" "${runtime_args[@]}"

for tool in linuxdeploy linuxdeploy-plugin-qt; do
    target="$tool_dir/$tool-x86_64.AppImage"
    if [[ ! -x "$target" ]]; then
        curl --fail --location --retry 3 \
            "https://github.com/linuxdeploy/$tool/releases/download/continuous/$tool-x86_64.AppImage" \
            --output "$target"
        chmod +x "$target"
    fi
done

# Keep Qt's supplied notices with the test package when building on Arch.
for module in qt6-base qt6-svg qt6-wayland; do
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
export OUTPUT="$repo_dir/dist/Magnite-0.1.0-test.1-x86_64.AppImage"
export VERSION=0.1.0-test.1
"$tool_dir/linuxdeploy-x86_64.AppImage" \
    --appdir "$app_dir" \
    --executable "$repo_dir/build/magnite-app" \
    --library "$(c++ -print-file-name=libstdc++.so.6)" \
    --library "$(cc -print-file-name=libgcc_s.so.1)" \
    --desktop-file "$repo_dir/packaging/magnite.desktop" \
    --icon-file "$repo_dir/app/resources/brand/symbol.svg" \
    --icon-filename magnite --plugin qt --output appimage

cd "$repo_dir/dist"
sha256sum Magnite-0.1.0-test.1-x86_64.AppImage > SHA256SUMS
cp "$repo_dir/packaging/TESTING.md" TESTING.md
printf 'Test AppImage: %s\n' "$OUTPUT"
