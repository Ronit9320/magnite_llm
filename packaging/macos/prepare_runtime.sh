#!/usr/bin/env bash
# Stage the isolated Python runtime + pinned ML dependencies into a Magnite.app.
#
# Wraps packaging/prepare_runtime.py (shared with the Linux recipe) with the macOS
# requirements file. python-build-standalone 3.12.12 comes from uv; llama-cpp-python is
# built from sdist with Metal (flags are set inside prepare_runtime.py for darwin).
#
# Usage: bash packaging/macos/prepare_runtime.sh [--fresh] [--destination DIR]
#   --fresh        delete any existing runtime first (clean retry)
#   --destination  directory that will contain runtime/, build-info/, licenses/
#                  (default: build/packaging/macos/runtime; build_app.sh clones it into the bundle)
# Safe to re-run: an existing runtime is resumed (deps re-resolved), not rebuilt.
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
destination="$repo_dir/build/packaging/macos/runtime"
fresh=0
while [[ $# -gt 0 ]]; do
    case "$1" in
        --fresh) fresh=1 ;;
        --destination) destination="$(cd -- "$2" 2>/dev/null && pwd || echo "$2")"; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done
[[ "$(uname -s)-$(uname -m)" == "Darwin-arm64" ]] || { echo "prepare_runtime.sh: needs an Apple-silicon Mac" >&2; exit 2; }
command -v uv >/dev/null || { echo "prepare_runtime.sh: install uv first" >&2; exit 2; }

export UV_CACHE_DIR="$repo_dir/build/packaging/uv-cache"   # same cache the dev venv used
mkdir -p "$destination" "$repo_dir/build/packaging"
if [[ $fresh -eq 1 ]]; then
    rm -rf "$destination/runtime" "$destination/build-info"
    # uv reuses a built llama-cpp-python wheel regardless of CMAKE_ARGS/CFLAGS, so a
    # fresh runtime must also drop it or changed build flags are silently ignored.
    uv cache clean llama-cpp-python
fi
resume=()
if [[ -d "$destination/runtime/python" ]]; then resume+=(--resume); fi

python3 "$repo_dir/packaging/prepare_runtime.py" \
    --destination "$destination" \
    --requirements "$repo_dir/packaging/macos/requirements-macos-arm64.txt" \
    ${resume[@]+"${resume[@]}"}
