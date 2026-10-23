# STATUS — macOS v0.1 packaging

**Read this file first, then the current phase section of `PLAN.md`. Execute only
that phase, then stop.** Do not re-read `RELEASE_CHECKLIST.md` or other long docs
unless the current phase names a specific section.

- Last updated: 2026-10-23 18:05 IST
- Branch: `release/v0.1-macos`
- Current phase: **Phase 6 — ML dependency matrix** (TODO)
- Last completed: **Phase 5 — `build_app.sh` produces a self-contained, ad-hoc-signed `Magnite.app` (1.1 GB); `verify_bundle.py` 0 failures;
  `smoke_bundle.sh` 6/6 PASS from a copy outside the checkout with a poisoned host env**
- Suggested model for the next phase: **Opus** (measurement design and failure interpretation, per the plan)

## Next exact commands (Phase 6 — read its section in `PLAN.md`)

```sh
cd /Users/architbhuta/magnite_llm
export MAGNITE_QT_ROOT=/Users/architbhuta/magnite_llm-master/.qt-tools/Qt/6.8.3/macos
# Bundle present? else: bash packaging/macos/build_app.sh   (72 s with an existing runtime)
R=build/packaging/macos/stage/Magnite.app/Contents/Resources/runtime/python/bin/python3
# write packaging/macos/ml_matrix.py, then run it with the BUNDLED interpreter (-I -B keeps the bundle signature intact):
"$R" -I -B packaging/macos/ml_matrix.py --out docs/release/macos/ML_MATRIX.md --cache build/packaging/macos/test-cache
codesign --verify --deep --strict build/packaging/macos/stage/Magnite.app   # must still pass afterwards
```

Phase 6 must also answer ISSUE-007 (do models in the shared `hub/blobs` store list/size/delete correctly with huggingface_hub 1.30.0?)
and keep in mind ISSUE-002 (≈10 s Metal shader compile on first llama.cpp load). bitsandbytes is **not** in the runtime: probe it in a
throwaway copy (`ditto` the runtime, `uv pip install` from `requirements-macos-quantization.txt`), never in the shipped one.

## Things learned in Phase 5 that later phases must respect

- **Pipeline:** `build_app.sh` = cmake → install bare bundle → `macdeployqt` (+ `libqoffscreen`) → `prepare_runtime.sh` into
  `build/packaging/macos/runtime` → APFS-clone into the bundle → `finalize_bundle.py` (clean → relink → thin → **sign last**) →
  `verify_bundle.py`. The bundle is rebuilt from scratch every run; never hand-edit `stage/Magnite.app` (it breaks the signature, and the
  kernel SIGKILLs the app — exit 137).
- **`smoke_bundle.sh <app>`** is the standard run check for Phases 7–10 (dmg copy, `/Applications`, second account). It checks the
  signature again after the run, which proves the app does not write into itself.
- `--fresh-runtime` re-downloads/rebuilds llama.cpp (≈3 min). A changed `CMAKE_ARGS`/`CFLAGS` **needs** it (uv caches the built wheel).
- Build the bundle for testers from a **clean committed tree** (`build.json` → `working_tree_modified`).
- `build-macos/Magnite.app` no longer has the Phase 4 runtime fixture (`build_app.sh` removes it, since install would copy 1.1 GB).
- Open: ISSUE-008 (transitive deps unpinned; certifi already drifted), ISSUE-009 (no Qt license texts) — both are release-gate items.

## Things learned in Phase 4 that later phases must respect

- Discovery order on macOS: `Contents/Resources/runtime` → `<MacOS>/runtime` → repo `.venv` → `python3`. Once a runtime dir exists
  there is **no fallback**: a broken runtime must fail loudly, not run the host Python.
- Prove the interpreter, don't infer it: `ps -axww -o ppid=,command=` (without `-ww` the command is truncated) filtered on the app's
  pid. When a harness appends to a log the app also writes, open the app's redirect with `>>`, or the app overwrites the harness lines.
  The pattern is in `build/packaging/macos/phase4-selftest.sh` (gitignored); Phase 5 should run the same poisoned-env selftest
  against the *installed* bundle copied outside the repo, because the build-tree bundle still has a dev-Qt rpath.
- `build-macos/Magnite.app/Contents/Resources/runtime` is an APFS clone fixture; rebuilding `build-macos` does not remove it.
- ISSUE-007: the pinned huggingface_hub 1.30.0 warns about `~/.cache/huggingface/hub/blobs` (written by a newer hub). Harmless here;
  Phase 6/9 decide whether it affects model listing/deletion.

## Things learned in Phase 3 that later phases must respect

- *(Resolved in Phase 5.)* The staged runtime was **not yet clean** (ISSUE-006): 3 universal `.so` files, 31 console scripts with a build-host shebang, build-host
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
  ISSUE-005 (test/`_remove_hf` symlink handling), ISSUE-007 (HF hub pin vs shared-blobs cache; Phase 6/9), ISSUE-008 (unpinned transitive deps; before Phase 7/12),
  ISSUE-009 (Qt license texts; Phase 12). ISSUE-006 resolved in Phase 5.

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
