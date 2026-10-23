# STATUS — macOS v0.1 packaging

**Read this file first, then the current phase section of `PLAN.md`. Execute only
that phase, then stop.** Do not re-read `RELEASE_CHECKLIST.md` or other long docs
unless the current phase names a specific section.

- Last updated: 2026-10-23 15:12 IST
- Branch: `release/v0.1-macos`
- Current phase: **Phase 3 — Relocatable Python runtime + pinned deps** (TODO)
- Last completed: **Phase 2 — `MAGNITE_MACOS_BUNDLE` CMake option; `Magnite.app` builds; plist/icon verified; plain build + 53 regressions unchanged**
- Suggested model for the next phase: **Sonnet** (switch to Opus if llama.cpp Metal fails twice)

## Next exact commands (Phase 3 — full detail in `PLAN.md`)

```sh
cd /Users/architbhuta/magnite_llm
export MAGNITE_QT_ROOT=/Users/architbhuta/magnite_llm-master/.qt-tools/Qt/6.8.3/macos
# 1. write packaging/macos/requirements-macos-arm64.txt (fork packaging/requirements-cpu.txt; bitsandbytes -> its own file)
# 2. write packaging/macos/prepare_runtime.sh and make a small sys.platform=="darwin" guarded edit to
#    packaging/prepare_runtime.py (macOS CMAKE_ARGS; no --torch-backend cpu)   <- SHARED FILE: log in DECISIONS.md,
#    then run bash packaging/macos/run_regressions.sh and re-run the plain CMake build, results in LOG.md
# 3. stage the runtime into the Phase 2 install tree
bash packaging/macos/prepare_runtime.sh --destination build/packaging/macos/stage/Magnite.app/Contents/Resources
R=build/packaging/macos/stage/Magnite.app/Contents/Resources/runtime/python/bin/python3
"$R" -I -B -c "import backend.server, llama_cpp, torch, transformers, peft, sentence_transformers, datasets; print(torch.__version__, torch.backends.mps.is_available())"
du -sh build/packaging/macos/stage/Magnite.app
```

If `build/packaging/macos/stage` is missing, recreate it: `bash packaging/macos/make_icon.sh`, then configure/build/install as
in `LOG.md` Phase 2 (`-DMAGNITE_MACOS_BUNDLE=ON`). Python 3.12.12 is the pinned runtime (verified downloadable for macos-aarch64).

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
