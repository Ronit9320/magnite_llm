# STATUS — macOS v0.1 packaging

**Read this file first, then the current phase section of `PLAN.md`. Execute only
that phase, then stop.** Do not re-read `RELEASE_CHECKLIST.md` or other long docs
unless the current phase names a specific section.

- Last updated: 2026-10-23 15:36 IST
- Branch: `release/v0.1-macos`
- Current phase: **Phase 4 — Bundle path discovery** (TODO) — **shared file `app/src/backend_manager.cpp`**
- Last completed: **Phase 3 — isolated Python 3.12.12 + 17 pinned deps staged (torch 2.14.0 MPS ✔, llama.cpp 0.3.35 Metal ✔); Linux behaviour of the shared script proved unchanged**
- Suggested model for the next phase: **Opus** (C++ path-discovery design judgment, per the plan)

## Next exact commands (Phase 4 — the corrected recipe is in `PLAN.md`; read it, do not use the old text)

```sh
cd /Users/architbhuta/magnite_llm
export MAGNITE_QT_ROOT=/Users/architbhuta/magnite_llm-master/.qt-tools/Qt/6.8.3/macos
# Prerequisites (recreate if missing):
#   build/packaging/macos/stage/Magnite.app/Contents/Resources/runtime   <- bash packaging/macos/prepare_runtime.sh
#   build-macos/Magnite.app                                             <- Phase 2 configure/build (-DMAGNITE_MACOS_BUNDLE=ON)
rm -rf build-macos/Magnite.app/Contents/Resources/runtime
cp -c -R build/packaging/macos/stage/Magnite.app/Contents/Resources/runtime build-macos/Magnite.app/Contents/Resources/runtime   # APFS clone
# 1. RED: run the selftest BEFORE editing (expected to fail: client looks for <MacOS>/runtime and falls back to bare python3)
# 2. edit app/src/backend_manager.cpp inside #ifdef Q_OS_MAC; 3. cmake --build build-macos -j4; 4. GREEN selftest with poisoned env
# 5. bash packaging/macos/run_regressions.sh ; cmake --build build -j4 && ./build/magnite-app --theme-check
```

Key design facts for Phase 4: `launchBackend()` currently checks `QDir(appDir + "/runtime")` (the Linux layout), then the dev `.venv`, then
`python3`. On macOS add `appDir + "/../Resources/runtime"` as the first probe, keep the `-I -B` / `MAGNITE_PACKAGED=1` / "never fall back"
contract, and skip the Linux-only `LD_LIBRARY_PATH` block. Do **not** touch `backend/src/backend/main.py` (ISSUE-001 is Phase 9's to measure).

## Things learned in Phase 3 that later phases must respect

- The staged runtime is **not yet clean** (ISSUE-006): 3 universal `.so` files, 31 console scripts with a build-host shebang, build-host
  paths in `_sysconfigdata`/`direct_url.json`. **Phase 5 fixes these before signing**; do not sign before thinning/scrubbing.
- `prepare_runtime.sh` is idempotent (resumes in ~12 s); `--fresh` wipes the runtime for a clean retry.
- Metal costs ~10.3 s at first load (ISSUE-002); MPS being *available* is not proof that training works on it (Phase 6).
- Every Mach-O in the runtime is minos ≤ 14.0 — ADR 0002 stands.
- Build from a **clean committed tree** for the tester package (`build.json` records `working_tree_modified`).

## Things learned in Phase 2 that later phases must respect

- In bundle mode the executable is **`Magnite.app/Contents/MacOS/Magnite`** (not `magnite-app`); the plain build keeps `magnite-app`.
- The **installed bundle cannot run until Phase 5** (`macdeployqt` adds `Contents/Frameworks`; install strips the dev rpath). Until
  then run checks on `build-macos/Magnite.app` (build-tree, keeps the dev-Qt rpath).
- `symbol.svg` has `@brandInk`/`@brandOrange` tokens — never rasterize it raw. Use `packaging/macos/make_icon.sh`.
- `CFBundleShortVersionString` is `0.1.0`; the label `0.1.0-test.1` lives in the `MagniteBuildLabel` plist key.

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
- Open, non-blocking issues: ISSUE-001 (no macOS parent-death signal; verify in Phase 9), ISSUE-004 (disk, 21 GiB free),
  ISSUE-005 (test/`_remove_hf` symlink handling), ISSUE-006 (runtime hygiene; fixed in Phase 5).

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
