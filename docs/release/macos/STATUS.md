# STATUS — macOS v0.1 packaging

**Read this file first, then the current phase section of `PLAN.md`. Execute only
that phase, then stop.** Do not re-read `RELEASE_CHECKLIST.md` or other long docs
unless the current phase names a specific section.

- Last updated: 2026-10-23 15:04 IST
- Branch: `release/v0.1-macos`
- Current phase: **Phase 2 — Bundle-aware CMake (shared file, APPLE-guarded)** (TODO)
- Last completed: **Phase 1 — `mac` fix merged; 53 regressions OK; theme-check 0 failures; `minos 14.0`; selftest PASS + `backend health OK`**
- Suggested model for the next phase: **Sonnet** (mechanical)

## Next exact commands (Phase 2 — full detail in `PLAN.md`)

```sh
cd /Users/architbhuta/magnite_llm
export MAGNITE_QT_ROOT=/Users/architbhuta/magnite_llm-master/.qt-tools/Qt/6.8.3/macos
# if .venv-macos is missing: uv venv --python 3.12 .venv-macos && uv pip install --python .venv-macos -e backend "pypdf==6.19.0"
bash packaging/macos/make_icon.sh        # to be written this phase -> build/packaging/macos/magnite.icns
# edit app/CMakeLists.txt (if(APPLE) + option(MAGNITE_MACOS_BUNDLE OFF)), add packaging/macos/Info.plist.in
cmake -S app -B build-macos -DCMAKE_PREFIX_PATH="$MAGNITE_QT_ROOT" -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 \
      -DMAGNITE_MACOS_BUNDLE=ON -DMAGNITE_BUILD_LABEL=0.1.0-test.1
cmake --build build-macos -j4
cmake --install build-macos --prefix build/packaging/macos/stage
cmake -S app -B build -DCMAKE_PREFIX_PATH="$MAGNITE_QT_ROOT" && cmake --build build -j4   # non-bundle build must still work
bash packaging/macos/run_regressions.sh     # shared-file edit => re-run + log in LOG.md
```

Phase 2 is a **shared-file edit** (`app/CMakeLists.txt`): additive, `if(APPLE)`-guarded,
log it in `DECISIONS.md`, and record the regression + plain-build results in `LOG.md`.

## Things learned in Phase 1 that later phases must respect

- **Always run regressions via `bash packaging/macos/run_regressions.sh`**, never bare
  `unittest`: macOS `/var` → `/private/var` makes 5 tests fail otherwise (ISSUE-005).
  It needs `.venv-macos` with `pypdf==6.19.0`.
- macOS has **no `timeout`** command; use a shell guard (background + `kill -0` loop).
- The dev `.venv-macos` is **unpinned** (llama-cpp-python 0.3.36, hf-hub 2.3.0, …). The
  pinned set (0.3.35, 1.30.0, …) is first exercised in Phase 3 — re-check Metal and the
  hf-hub-dependent tests there.
- llama.cpp built with Metal **by default** with no Metal compiler (embedded shaders), but
  costs ~10 s of shader compilation at load (ISSUE-002 update) — measure in Phase 6.
- `build-macos/` is the packaging build dir; `build/` is the untouched dev build.

## Known blockers

- **Phase 11 (Developer ID signing + notarization) is BLOCKED.** Needs: Apple Developer
  Program enrollment, a Developer ID Application certificate in the login keychain, the
  Team ID, and a `notarytool store-credentials` profile. See `adr/0005-signing-approach.md`.
- **Phase 10 needs a second macOS user account** on this M1 (the agreed clean-install
  proxy). Its name must be supplied, or the maintainer runs `CLEAN_INSTALL.md` there.
- Open, non-blocking issues: ISSUE-001 (no macOS parent-death signal; verify in Phase 9),
  ISSUE-004 (disk headroom, 23 GiB free), ISSUE-005 (test/`_remove_hf` symlink handling).

## Standing rules for every session

- One phase per session. Stop at the acceptance check and say it is safe to `/clear`.
- Update `STATUS.md`, `LOG.md` and `PLAN.md` statuses as work happens, not at the end.
- Shared-file edits (`app/CMakeLists.txt`, `app/src/backend_manager.cpp`,
  `packaging/prepare_runtime.py`) must be additive and platform-guarded, logged in
  `DECISIONS.md`, and followed by re-running the offline regressions **and** the plain
  CMake build, with the result in `LOG.md`.
- Never commit `dist/` artifacts, model weights, build outputs or secrets.
- Never modify the Linux packaging or `dist/` AppImage; they are read-only references.
- Tick only macOS lines in `RELEASE_CHECKLIST.md`, only with logged evidence.
- Nothing is "verified" unless the check ran in that session and is in `LOG.md`.
