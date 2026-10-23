# STATUS — macOS v0.1 packaging

**Read this file first, then the current phase section of `PLAN.md`. Execute only
that phase, then stop.** Do not re-read `RELEASE_CHECKLIST.md` or other long docs
unless the current phase names a specific section.

- Last updated: 2026-10-23 19:12 IST
- Branch: `release/v0.1-macos`
- Current phase: **Phase 8 — Functional verification of the installed app** (TODO)
- Last completed: **Phase 7 — `dist/Magnite-0.1.0-test.1-arm64.dmg` (321 MB, clean commit `5a04358`, SHA-256 `f72686db…d3ab43`
  in `dist/SHA256SUMS-macos`); `check_dmg.sh` 10/10 PASS from the read-only volume**
- Suggested model for the next phase: **Sonnet** (running and recording; per the plan)

## ⚠ Decisions needed from the maintainer (not blocking Phase 7)

1. **ISSUE-010 — blocks Phase 8.** "Use 4-bit QLoRA" is on by default, and the TRAINING/ADAPTER selftests send `"4bit"`; the macOS
   runtime has no bitsandbytes, so both fail. Options are in `ISSUES.md` (recommended: a capability-aware UI and selftests). This is a
   shared-code change, so it needs approval.
2. **ISSUE-007.** Raise the `huggingface_hub` pin to ≥ 1.33 (both platforms), or document the disk leak on delete.
3. **ISSUE-008.** Lock the transitive Python deps before the tester build (affects Linux too).

## Next exact commands (Phase 8 — read its section in `PLAN.md`)

```sh
cd /Users/architbhuta/magnite_llm
D=dist/Magnite-0.1.0-test.1-arm64.dmg; (cd dist && shasum -a 256 -c SHA256SUMS-macos)   # expect: OK
M=$(mktemp -d); hdiutil attach "$D" -readonly -nobrowse -mountpoint "$M"
ditto "$M/Magnite.app" "$TMPDIR/Magnite-verify.app"; hdiutil detach "$M"
A="$TMPDIR/Magnite-verify.app/Contents/MacOS/Magnite"
export MAGNITE_HOME="$TMPDIR/magnite-verify-home" HF_HOME="$PWD/build/packaging/macos/test-cache" QT_QPA_PLATFORM=offscreen
# then the plan's list: --theme-check, --ui-check, --selftest, the five MAGNITE_SELFTEST_* runs, live_workflow.py, --screenshot
```

Phase 8 cautions:
- **Expected failures until ISSUE-010 is decided:** `MAGNITE_SELFTEST_TRAINING` and `MAGNITE_SELFTEST_ADAPTER` send `quantization: "4bit"`,
  and that fails ("requires bitsandbytes") — already reproduced on the bundle in Phase 6. Record them as ISSUE-010, not as new bugs. If the
  maintainer approves a fix first, rebuild (`build_app.sh` → `build_dmg.sh`) before Phase 8.
- Use `HF_HOME=build/packaging/macos/test-cache` so the selftests never touch the real `~/.cache/huggingface` (ISSUE-007).
- Run each selftest with a fresh `MAGNITE_HOME` and a time guard (macOS has no `timeout`; see `smoke_bundle.sh`).
- `backend/tests/live_workflow.py` exists; run it with the bundled interpreter `-I -B` as the plan says, and check what it needs first.

## Things learned in Phase 7 that later phases must respect

- `bash packaging/macos/check_dmg.sh <dmg>` is the standard dmg check (mount read-only → layout → `smoke_bundle.sh` → detach).
- Compare mount paths with `cd -P` / `/private/var`: `TMPDIR` is under the `/var` symlink (ISSUE-005 again).
- `hdiutil` is deprecated on macOS 27 but works; warnings are expected in logs.
- Build the tester image only from a clean commit: `build_app.sh` then `build_dmg.sh` (it warns on a modified tree).

## Things learned in Phase 6 that later phases must respect

- `ML_MATRIX.md` is the **only** source of ML claims for README/TESTING/release notes. Re-run:
  `"$R" -I -B packaging/macos/ml_matrix.py --out docs/release/macos/ML_MATRIX.md --cache build/packaging/macos/test-cache`
  (~10 min; models are cached in the 1.5 GB test cache; `--only cell,cell` for a subset).
- Phase 8's TRAINING and ADAPTER selftests **will fail** until ISSUE-010 is resolved. That is expected, not a packaging bug.
- First GPU chat on a fresh user pauses ~10 s (Metal shader compile, once per user); Phase 10 should time it on the second account.
- MPS makes LoRA ~23× faster than CPU; there is no reason to add a CPU-training fallback for Apple silicon.
- Benchmark tiny models with care: on a 135M GGUF, the CPU beats Metal 3×; on 1.5B they are roughly equal on a base M1.

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
  ISSUE-005 (test/`_remove_hf` symlink handling), ISSUE-007 (hub 1.30 leaks disk on delete for shared-blobs caches; decision), ISSUE-008 (unpinned transitive deps; before Phase 7/12),
  ISSUE-009 (Qt license texts; Phase 12),
  **ISSUE-010 (default 4-bit QLoRA fails on macOS; blocks Phase 8)**. ISSUE-006 resolved in Phase 5.

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
