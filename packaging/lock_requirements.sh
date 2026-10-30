#!/usr/bin/env bash
# Regenerate the full dependency locks (every transitive package pinned) for the packaged runtimes.
#
# Usage: bash packaging/lock_requirements.sh
# Inputs are the hand-edited direct pins plus backend/pyproject.toml; outputs are constraints files that
# packaging/prepare_runtime.py passes to `uv pip install -c`, so two builds of one commit get the same packages.
#   packaging/requirements-cpu.txt              -> packaging/constraints-cpu.txt              (Linux x86_64, CPU torch)
#   packaging/macos/requirements-macos-arm64.txt -> packaging/macos/constraints-macos-arm64.txt (macOS 14+ arm64)
# Re-run after changing a direct pin, and commit the requirements and constraints changes together.
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
command -v uv >/dev/null || { echo "lock_requirements.sh: install uv first" >&2; exit 2; }
export UV_CACHE_DIR="$repo_dir/build/packaging/uv-cache"
cd "$repo_dir"

uv pip compile packaging/requirements-cpu.txt backend/pyproject.toml \
    --python-version 3.12 --python-platform x86_64-manylinux_2_28 --torch-backend cpu \
    --quiet -o packaging/constraints-cpu.txt
# The torch wheel tag (macosx_14_0_arm64) sets the floor; without it uv targets an older macOS.
MACOSX_DEPLOYMENT_TARGET=14.0 uv pip compile packaging/macos/requirements-macos-arm64.txt backend/pyproject.toml \
    --python-version 3.12 --python-platform aarch64-apple-darwin \
    --quiet -o packaging/macos/constraints-macos-arm64.txt

for lock in packaging/constraints-cpu.txt packaging/macos/constraints-macos-arm64.txt; do
    printf '%s: %s packages\n' "$lock" "$(grep -c '==' "$lock")"
done
