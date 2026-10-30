#!/usr/bin/env bash
# Run the full functional acceptance set against an installed/copied Magnite.app (Phase 8; reused in Phase 10).
#
# Usage: bash packaging/macos/verify_installed.sh <path/to/Magnite.app> <log-dir> [hf-home]
#   hf-home  HF_HOME for model downloads (default: <log-dir>/hf-home). Point it at a shared test cache
#            to avoid re-downloading; never at ~/.cache/huggingface (ISSUE-007).
# Every step runs with a poisoned host Python env, a fresh MAGNITE_HOME, a time limit, and its own log.
# Prints one PASS/FAIL line per step and a summary; exit 0 only if every step passed.
set -uo pipefail
repo_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
app="$(cd -- "$1" && pwd)"
mkdir -p "$2"; log_dir="$(cd -- "$2" && pwd)"
hf_home="${3:-$log_dir/hf-home}"; mkdir -p "$hf_home"; hf_home="$(cd -- "$hf_home" && pwd)"
exe="$app/Contents/MacOS/$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$app/Contents/Info.plist")"
python="$app/Contents/Resources/runtime/python/bin/python3"
limit="${MAGNITE_VERIFY_TIMEOUT:-900}"
failures=0
summary=()

# run_step <name> <pass-regex> <command...>
run_step() {
    local name="$1" pattern="$2"; shift 2
    local log="$log_dir/$name.log" home="$log_dir/home-$name" start rc
    rm -rf "$home"; : >"$log"; start=$(date +%s)
    env -i HOME="$HOME" PATH=/usr/bin:/bin PYTHONHOME=/nonexistent PYTHONPATH=/nonexistent \
        MAGNITE_HOME="$home" HF_HOME="$hf_home" QT_QPA_PLATFORM=offscreen \
        HF_HUB_DISABLE_TELEMETRY=1 TOKENIZERS_PARALLELISM=false ${STEP_ENV:-} "$@" >>"$log" 2>&1 &
    local pid=$! waited=0
    while kill -0 "$pid" 2>/dev/null && (( waited < limit * 2 )); do sleep 0.5; waited=$((waited + 1)); done
    if kill -0 "$pid" 2>/dev/null; then kill "$pid"; echo "[verify] TIMEOUT after ${limit}s" >>"$log"; fi
    wait "$pid"; rc=$?
    sleep 1
    local orphans; orphans=$(pgrep -fl "$app/Contents/Resources/runtime" || true)
    local secs=$(( $(date +%s) - start )) line
    if [[ $rc -eq 0 ]] && grep -Eq "$pattern" "$log" && [[ -z "$orphans" ]]; then
        line="PASS  $name (${secs}s): $(grep -E "$pattern" "$log" | tail -1 | cut -c1-120)"
    else
        failures=$((failures + 1))
        local why; why=$(grep -E '\[(selftest|ui-check)\] FAIL|FAIL:|Error|error:|Traceback|TIMEOUT' "$log" | head -2 | tr '\n' ' ' | cut -c1-220)
        [[ -n "$orphans" ]] && why="orphaned backend left running; $why" && pkill -f "$app/Contents/Resources/runtime"
        line="FAIL  $name (${secs}s, exit $rc): ${why:-no pass marker; see $log}"
    fi
    echo "$line"; summary+=("$line")
}

run_step theme-check '^\[theme-check\] 0 failure\(s\)' "$exe" --theme-check
run_step ui-check '^\[ui-check\] PASS' "$exe" --ui-check "$log_dir/ui-check-captures"
run_step selftest '^\[selftest\] PASS' "$exe" --selftest
for kind in PULL INFERENCE TRAINING ADAPTER RAG; do
    STEP_ENV="MAGNITE_SELFTEST_$kind=1" run_step "selftest-$(echo "$kind" | tr 'A-Z' 'a-z')" '^\[selftest\] PASS' "$exe" --selftest
done
run_step live-workflow '^PASS ' "$python" -I -B "$repo_dir/backend/tests/live_workflow.py"
rm -rf "$log_dir/screenshots"; mkdir -p "$log_dir/screenshots"
run_step screenshots '.' "$exe" --screenshot "$log_dir/screenshots"
shots=$(ls "$log_dir/screenshots"/*.png 2>/dev/null | wc -l | tr -d ' ')
echo "      screenshots captured: $shots"

if codesign --verify --deep --strict "$app" 2>/dev/null; then echo "PASS  signature still valid after all steps"
else echo "FAIL  signature broken after the run (the app wrote into its bundle)"; failures=$((failures + 1)); fi
echo "$failures failure(s); logs in $log_dir"
exit $((failures > 0))
