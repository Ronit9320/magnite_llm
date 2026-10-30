#!/usr/bin/env bash
# Phase 10 helper: runs the CLEAN_INSTALL.md commands and saves their output to auto-results.txt,
# so the tester only has to click and take screenshots.
# Usage (in the second account): bash collect.sh start | running | finish
set -uo pipefail
kit="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
out="$kit/auto-results.txt"
section() { printf '\n===== %s (%s, user %s) =====\n' "$1" "$(date '+%Y-%m-%d %H:%M:%S')" "$(whoami)" >>"$out"; }

case "${1:-}" in
start)
    section "1. Environment"
    { sw_vers; whoami; id; echo "PATH=$PATH"
      for t in python3 brew uv; do printf '%s: ' "$t"; command -v "$t" || echo "not found"; done
      ls -d ~/.magnite ~/.cache/huggingface /opt/homebrew 2>&1
      ls -d /Applications/Magnite.app 2>&1; } >>"$out" 2>&1
    section "2. Quarantine on the downloaded copy"
    cp "$kit/Magnite-download.dmg" ~/Downloads/
    xattr -p com.apple.quarantine ~/Downloads/Magnite-download.dmg >>"$out" 2>&1 || echo "NO QUARANTINE FLAG" >>"$out"
    # Screenshots of the dialogs land in the kit folder, where the developer account can read them.
    defaults write com.apple.screencapture location "$kit"
    killall SystemUIServer 2>/dev/null
    open ~/Downloads
    echo
    echo "Done. Next, in the Finder window that just opened:"
    echo "  1. Double-click Magnite-download.dmg. Screenshot any dialog (Shift+Cmd+4, Space, click it)."
    echo "  2. Drag Magnite onto Applications, then eject the image."
    echo "  3. Open Magnite from Applications. When macOS blocks it: screenshot, click Done,"
    echo "     then System Settings -> Privacy & Security -> Open Anyway (screenshot each dialog)."
    echo "  4. When the Magnite window is open, run:  bash $kit/collect.sh running"
    ;;
running)
    section "5. Processes while Magnite is open"
    ps -axww -o pid,user,command | grep -E "MacOS/Magnite|backend.main" | grep -v grep >>"$out" 2>&1 \
        || echo "no Magnite processes found" >>"$out"
    echo
    echo "Done. Next, in Magnite:"
    echo "  1. Library: download Segilmez06/SmolLM2-135M-Instruct-Q4_K_M-GGUF"
    echo "  2. Playground: send 'What are the three primary colours?' then 'Name one more colour.'"
    echo "     Note roughly how many seconds each took before the first words appeared."
    echo "  3. Quit Magnite (Cmd+Q), then run:  bash $kit/collect.sh finish"
    ;;
finish)
    if pgrep -u "$(id -u)" -f "MacOS/Magnite" >/dev/null; then
        echo "Magnite is still open. Quit it with Cmd+Q, then run this again."; exit 1
    fi
    section "7. Automated check"
    echo "Running the automated check (takes about a minute)..."
    bash "$kit/smoke_bundle.sh" /Applications/Magnite.app ~/magnite-smoke >>"$out" 2>&1
    pgrep -u "$(id -u)" -fl backend.main >>"$out" || echo "nothing running" >>"$out"
    mkdir -p "$kit/logs-$(whoami)"
    cp ~/magnite-smoke/*.log "$kit/logs-$(whoami)/" 2>/dev/null
    section "8. Clean up"
    defaults delete com.apple.screencapture location 2>/dev/null
    killall SystemUIServer 2>/dev/null
    rm -rf ~/.magnite ~/magnite-smoke ~/Downloads/Magnite-download.dmg
    echo "cleaned ~/.magnite, ~/magnite-smoke and the downloaded dmg" >>"$out"
    tail -n 12 "$out"
    echo
    echo "Done. Last steps: drag Magnite from Applications to the Trash, empty the Trash,"
    echo "then switch back to your main account and say 'Phase 10 results are in'."
    ;;
*)
    echo "usage: bash $0 start | running | finish"; exit 2
    ;;
esac
