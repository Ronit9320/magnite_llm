# ISSUES — macOS v0.1 packaging

Failures, limitations and workarounds. Each gets a root cause once known. Open
issues must be reflected in `STATUS.md` blockers when they gate a phase.

---

## ISSUE-001 — No parent-death signal on macOS: force-killed GUI can orphan the backend

- Status: **OPEN — to be verified in Phase 9, not fixed as part of packaging**
- Found: 2026-10-23 (code reading during the Phase 0 feasibility check)
- Severity: functional bug, user-visible (stray `python` process holding a port and RAM)

`backend/src/backend/main.py:14` defines `_enable_parent_death_signal()`, whose
docstring states the intent plainly: "Ask the kernel to SIGTERM us if our parent
process dies, so a crashed or force-killed GUI cannot leave an orphaned backend
behind." The implementation is guarded by `if sys.platform == "linux" and
os.name == "posix"` and uses Linux's `prctl(PR_SET_PDEATHSIG)`. macOS has no
`prctl`, so on macOS the function is a no-op and the stated protection does not
exist.

The cooperative path is unaffected: `BackendManager::stop()` terminates and then
kills the child, so a normal quit is clean. The gap is the non-cooperative path —
`kill -9` of the GUI, or a GUI crash — which `RELEASE_CHECKLIST.md` names under
"backend process cleanup" and which the existing audit already lists as
unverified ("forced-process-death coverage remain unverified").

Not fixed here, per the working rule to report functional bugs rather than
silently change application source during packaging. Phase 9 will measure the
actual behaviour and record it.

Possible fixes, if the maintainer wants one later (each platform-guarded):

1. A kqueue `EVFILT_PROC`/`NOTE_EXIT` watcher on the parent PID in the backend,
   the closest macOS equivalent to `PR_SET_PDEATHSIG`.
2. A small watchdog thread polling `os.getppid()` and exiting when it changes to
   1 — simplest and portable, with poll-interval latency.
3. Client-side only: a `atexit`/signal-handler guarantee in the GUI, which does
   not help on `SIGKILL` and so does not actually close the gap.

Recommendation if asked: option 2 for v0.1 (small, guarded, testable), option 1
if a cleaner solution is wanted.

---

## ISSUE-002 — No Metal shader compiler on the build host

- Status: **OPEN — mitigation planned in Phase 3**
- Found: 2026-10-23
- Severity: build-time constraint

`xcrun -sdk macosx metal --version` fails with "unable to find utility
\"metal\", not a developer tool or in PATH" because only Command Line Tools 14.3
are installed (no `/Applications/Xcode.app`). A default `GGML_METAL=ON` build of
`llama-cpp-python` compiles `.metal` sources into a `default.metallib` and needs
that utility.

Planned mitigation: `-DGGML_METAL_EMBED_LIBRARY=ON`, which embeds the shader
source and lets the Metal driver compile it at runtime, requiring no build-time
toolchain. If that fails twice, fall back to `-DGGML_METAL=OFF` (CPU-only
llama.cpp) and record it as a documented limitation rather than installing Xcode
for v0.1. Either way the outcome is measured in Phase 6, not assumed.

---

## ISSUE-003 — Unsigned build will trip Gatekeeper on download

- Status: **ACCEPTED LIMITATION for v0.1** (see `adr/0005-signing-approach.md`)
- Found: 2026-10-23
- Severity: usability, public-distribution blocker

`security find-identity -v -p codesigning` reports `0 valid identities found`, so
the v0.1 `.dmg` can only be ad-hoc signed. A downloaded copy carries
`com.apple.quarantine`, and macOS will refuse the first launch until the user
goes to System Settings → Privacy & Security → **Open Anyway** (the older
right-click → Open bypass no longer applies on recent macOS). App Translocation
may also run the bundle from a random read-only path until it is moved to
`/Applications`.

Consequences, all already in the plan: the artifact is labelled an unsigned
testing build; `CLEAN_INSTALL.md` (Phase 10) records the exact dialog wording and
the steps that clear it; the macOS public-distribution checklist line stays
unticked; Phase 11 stays BLOCKED.

---

## ISSUE-004 — Build-host disk headroom is tight

- Status: **OPEN — monitor each phase**
- Found: 2026-10-23
- Severity: could stall the plan if ignored

`df -h` shows ~20 GiB free (91% used) on the data volume, and 8 GiB of RAM. The
staged bundle will carry Qt frameworks plus torch and llama.cpp, and the uv cache
and a test HF model cache sit under `build/packaging/`. Mitigation: record
`du -sh` at each phase in `LOG.md`, keep caches under `build/packaging/`, and
prune the uv cache after a successful runtime build. 8 GiB of RAM also caps which
models can be used for real-model verification — tiny toy models remain the
default, which is a limitation to state, not to hide.
