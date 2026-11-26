#!/bin/bash
# Installed-app update for macOS, bundled in the client as :/updates/install-macos.sh.
#
# The client has already verified the Ed25519-signed manifest and the archive's
# SHA-256, unpacked the new bundle beside the installed one, and stopped the
# backend. It starts this script detached, waits for "ready", then quits.
#
# Usage: install-update.sh INSTALLED.app NEW.app TRANSACTION_DIR PARENT_PID VERSION
#
# The previous bundle stays in TRANSACTION_DIR/previous.app until the next update,
# so a failed start is rolled back and a later problem can be recovered by hand.
# Compatible with the system Bash 3.2.
set -u

if [[ $# -ne 5 ]]; then echo "usage: install-update.sh INSTALLED NEW TRANSACTION PID VERSION" >&2; exit 2; fi
installed=$1 incoming=$2 transaction=$3 parent=$4 version=$5

status() { printf '%s' "$1" > "$transaction/status.tmp" && mv -f "$transaction/status.tmp" "$transaction/status"; }
log() { printf '%s %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)" "$*" >> "$transaction/installer.log"; }
cancelled() { [[ -e "$transaction/cancel" ]]; }

case $installed in /*.app) ;; *) echo "installed path must be an absolute .app" >&2; exit 2 ;; esac
case $parent in ''|*[!0-9]*) echo "invalid parent process id" >&2; exit 2 ;; esac
if [[ ! -d $installed || -L $installed || ! -d $incoming || ! -d $transaction ]]; then
    echo "missing bundle or transaction directory" >&2; exit 2
fi
# The transaction directory is the client's private staging folder beside the app.
case $transaction in "$(dirname "$installed")"/.magnite-update-*) ;; *) echo "unexpected transaction path" >&2; exit 2 ;; esac
previous="$transaction/previous.app"
health="$transaction/healthy"
# Polling budget in 0.2 s ticks (120 s); tests shorten it.
ticks=${MAGNITE_UPDATE_TICKS:-600}
case $ticks in ''|*[!0-9]*) ticks=600 ;; esac

status ready
log "waiting for process $parent to exit before installing $version"
for _ in $(seq 1 "$ticks"); do
    if cancelled; then exit 3; fi
    kill -0 "$parent" 2>/dev/null || break
    sleep 0.2
done
if kill -0 "$parent" 2>/dev/null; then status "cancelled: application did not exit"; exit 3; fi
if cancelled; then exit 3; fi

# Both renames stay on one filesystem, so each step leaves a complete bundle in place.
if ! mv "$installed" "$previous"; then
    status "failed: could not move the installed app (check System Settings > Privacy & Security > App Management)"
    log "rename of the installed bundle failed"
    open "$installed"
    exit 4
fi
if ! mv "$incoming" "$installed"; then
    mv "$previous" "$installed"
    status "failed: could not place the new app; the previous version was kept"
    open "$installed"
    exit 4
fi
# The client downloads without a quarantine flag; clear any that a tool added.
xattr -dr com.apple.quarantine "$installed" 2>/dev/null || true
status "installed; checking startup"
log "installed $version, starting it"

rm -f "$health"
open -n "$installed" --args --update-health-file "$health"
for _ in $(seq 1 "$ticks"); do
    if [[ -f $health && "$(cat "$health" 2>/dev/null)" == "$version" ]]; then
        status "success: $version"
        log "new version became ready"
        exit 0
    fi
    sleep 0.2
done

# The new version did not become ready: stop it and its backend, then restore.
log "new version did not become ready; rolling back"
pkill -f "$installed/Contents/" 2>/dev/null
for _ in $(seq 1 50); do pgrep -f "$installed/Contents/" >/dev/null || break; sleep 0.2; done
pkill -9 -f "$installed/Contents/" 2>/dev/null
if mv "$installed" "$transaction/failed.app" && mv "$previous" "$installed"; then
    status "rolled back: $version did not become ready"
    open "$installed"
    exit 6
fi
status "recovery required: the previous app is $previous"
exit 5
