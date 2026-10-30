#!/usr/bin/env bash
# Smoke-test a finished Magnite.app wherever it lives (a copy outside the checkout, /Applications, ...).
#
# Usage: bash packaging/macos/smoke_bundle.sh <path/to/Magnite.app> [log-dir]
# Runs, with a host Python environment that is deliberately broken (PYTHONHOME/PYTHONPATH poisoned,
# PATH without Homebrew/uv): signature check, --theme-check, the bare --selftest (recording which
# interpreter the backend ran under), an orphan check, then the signature check again to prove
# the app did not write into its own bundle. Exit 0 only if everything passed.
set -uo pipefail
app="$(cd -- "$1" && pwd)"
log_dir="${2:-${TMPDIR%/}/magnite-smoke}"
mkdir -p "$log_dir"
exe="$app/Contents/MacOS/$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$app/Contents/Info.plist")"
home="$log_dir/magnite-home"
rm -rf "$home"
failures=0
fail() { echo "FAIL  $*"; failures=$((failures + 1)); }
pass() { echo "PASS  $*"; }
run_clean() {
    env -i HOME="$HOME" PATH=/usr/bin:/bin PYTHONHOME=/nonexistent PYTHONPATH=/nonexistent \
        MAGNITE_HOME="$home" QT_QPA_PLATFORM=offscreen "$@"
}

if codesign --verify --deep --strict --verbose=4 "$app" 2>"$log_dir/codesign-before.log"; then pass "signature before run"
else fail "signature before run (see $log_dir/codesign-before.log)"; fi

run_clean "$exe" --theme-check >"$log_dir/theme-check.log" 2>&1
if grep -q '^\[theme-check\] 0 failure(s)' "$log_dir/theme-check.log"; then pass "--theme-check: 0 failure(s)"
else fail "--theme-check (see $log_dir/theme-check.log)"; fi

: >"$log_dir/selftest.log"
run_clean "$exe" --selftest >>"$log_dir/selftest.log" 2>&1 &
pid=$!
child=""
for _ in $(seq 1 360); do
    kill -0 "$pid" 2>/dev/null || break
    if [[ -z "$child" ]]; then
        # The backend is a grandchild of $pid (function subshell -> app); match its unique path instead.
        child=$(ps -axww -o command= | grep -F "$app/Contents/Resources/runtime/" | grep -F "backend.main" | grep -v grep || true)
    fi
    sleep 0.5
done
if kill -0 "$pid" 2>/dev/null; then kill "$pid"; fail "--selftest timed out after 180 s"; fi
wait "$pid"
if grep -q '^\[selftest\] PASS' "$log_dir/selftest.log" && grep -q 'backend health OK' "$log_dir/selftest.log"; then
    pass "--selftest: $(grep '^\[selftest\] PASS' "$log_dir/selftest.log"), backend health OK"
else
    fail "--selftest (see $log_dir/selftest.log)"
fi
expected="$app/Contents/Resources/runtime/python/bin/python3 -B -I -m backend.main"
if [[ "$child" == *"$expected"* ]]; then pass "backend interpreter: bundled python3 -B -I"
else fail "backend interpreter: '${child:-not seen}' (expected '$expected')"; fi

sleep 2
orphans=$(pgrep -fl "$app/Contents/Resources/runtime" || true)
if [[ -z "$orphans" ]]; then pass "no backend left running"; else fail "orphaned backend: $orphans"; fi

if codesign --verify --deep --strict --verbose=4 "$app" 2>"$log_dir/codesign-after.log"; then pass "signature after run (bundle unmodified)"
else fail "signature after run (see $log_dir/codesign-after.log)"; fi

echo "$failures failure(s); logs in $log_dir"
exit $((failures > 0))
