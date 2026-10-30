#!/usr/bin/env bash
# Run the offline backend regression suite on macOS.
#
# TMPDIR is pinned to its symlink-resolved form because macOS's /var is a symlink
# to /private/var; without this 5 tests fail for a path-spelling reason that is
# unrelated to the code under test (docs/release/macos/ISSUES.md, ISSUE-005).
#
# Usage: bash packaging/macos/run_regressions.sh [log-file]
set -euo pipefail
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
python="$repo_dir/.venv-macos/bin/python"
log="${1:-$repo_dir/build/packaging/regressions.log}"

[[ -x "$python" ]] || { echo "Missing $python — create it: uv venv --python 3.12 .venv-macos && uv pip install --python .venv-macos -e backend 'pypdf==6.19.0'" >&2; exit 2; }
mkdir -p "$(dirname "$log")"
resolved_tmp="$("$python" -c 'import os, tempfile; print(os.path.realpath(tempfile.gettempdir()))')"

set +e
TMPDIR="$resolved_tmp" PYTHONPATH="$repo_dir/backend/src" \
    "$python" -m unittest discover -s "$repo_dir/backend/tests" -v >"$log" 2>&1
status=$?
set -e
tail -4 "$log"
echo "log: $log (exit $status)"
exit "$status"
