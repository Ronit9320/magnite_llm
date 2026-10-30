# STATUS — macOS v0.1 packaging

> Integration note, 2026-10-30: this file records the earlier Mac-only work.
> The changes described as uncommitted below were committed through `8337e65`
> and `86950a8` and pushed to `mac`. The owner has now started the
> [multi-platform integration plan](../../development/MULTIPLATFORM_RELEASE_PLAN.md)
> on `release/integration`. Its instructions and root AGENTS.md govern current
> work; the old no-merge/no-commit and one-phase rules below are historical.
> Earlier Mac results do not verify the integrated source. New native checks
> must rebuild the bundle/DMG using the canonical identity, not the old test label.

**Read this file first, then the current phase section of `PLAN.md`. Execute only
that phase, then stop.** Do not re-read `RELEASE_CHECKLIST.md` or other long docs
unless the current phase names a specific section.

- Last updated: 2026-10-30
- Branch: `release/v0.1-macos`. The fixes and docs up to Phase 10 are committed as `8337e65` (pushed to `mac`). Phase 12's edits are
  **uncommitted**; the standing no-commit rule below dates from 2026-10-24 and was overridden by that commit, so ask before committing.
- Current phase: **none open.** Phase 12 is DONE; Phase 11 (Developer ID signing and notarization) is DEFERRED by the maintainer (2026-10-30); users follow `docs/MACOS_FIRST_LAUNCH.md`.
- Last completed: **Phase 12 (2026-10-30):** `shasum -c` OK; 65 regressions OK; wrote `V0_1_MACOS_TESTING.md` and `packaging/macos/TESTING.md`;
  added macOS sections to `packaging/README.md` and `README.md`; ticked checklist lines 1–2 (line 3 stays open, line 4 marked BLOCKED).
- Before a tester build: rebuild from a clean commit (`build_app.sh` → `build_dmg.sh`). The current dmg's `build.json` says commit `ef173fd`,
  modified tree.

## Decisions

All fixes (ISSUE-001/005/007/008/009/010/011) are in place and **uncommitted**; see `DECISIONS.md` and `LOG.md`.
- ISSUE-009 needs the maintainer's **legal sign-off** on the bundled Qt notices before anything ships.
- ISSUE-002 and ISSUE-003 are accepted limitations. Phase 10 did not reproduce the 10 s pause at chat time, but model-load time was not
  measured. The Gatekeeper wording is still uncaptured.
- ISSUE-004: keep an eye on disk space.
- Current dmg: `48050836…` (320 MB); acceptance 11/11 (developer account), smoke 6/6 (second account).

## Next exact commands (Phase 12 — read its section in `PLAN.md`)

```sh
cd /Users/architbhuta/magnite_llm
(cd dist && shasum -a 256 -c SHA256SUMS-macos)
# write docs/release/macos/V0_1_MACOS_TESTING.md (shape of docs/release/V0_1_LINUX_TESTING.md), packaging/macos/TESTING.md,
# a macOS section in packaging/README.md, README.md verification matrix; tick only evidenced macOS lines in RELEASE_CHECKLIST.md
```

Phase 12 cautions:
- **Do not run the plan's `git add`/`git commit` lines.** The no-commit rule below overrides them.
- `ML_MATRIX.md` is the only source of ML claims.
- Do **not** quote Gatekeeper wording that was never captured. Describe the flow (blocked → Privacy & Security → Open Anyway), or have
  the maintainer capture the wording first: reinstall from the kit in `tester` and screenshot it.
- Put the "not claimed" items into V0_1_MACOS_TESTING.md:
  - no notarization;
  - no truly clean machine (machine-wide Homebrew/python3 were visible to the proxy account);
  - no Intel;
  - no macOS 14/15;
  - the cross-account API check (§7b) not run.
- `TESTING.md`: "the first time a model is loaded may pause ~10 s while macOS prepares GPU shaders". Also the force-quit note, and the
  prefs domain `com.magnite-local.Magnite LLM` for uninstall.

## Things learned in Phase 10 that later phases must respect

- `packaging/macos/clean_install_collect.sh` (staged as `collect.sh`) records everything a tester would paste. Ask the tester only for
  clicks, screenshots and timings.
- Screenshots: no files reached the kit folder. It is unclear whether any were taken, or whether the `screencapture location`
  redirect failed. If wording matters, ask the tester to type the dialog text into the chat as well.
- Homebrew under `/opt/homebrew` and `/usr/local/bin/python3` are visible to every account. A second account is only a partial proxy for a clean Mac.
- The Metal compile cost lands at **model load**. Time the load, not just the first reply, and keep the backend log before cleanup.

## Things learned in Phase 9 that later phases must respect

- The preferences domain is `com.magnite-local.Magnite LLM` (QSettings), not the bundle id. Use it in `TESTING.md` uninstall notes.
- A normal quit is clean; a force-quit is not (ISSUE-001). `TESTING.md` must tell testers to check Activity Monitor for a stray
  `python3` after a crash until it is fixed.
- Launch like a user with `open -n -a /Applications/Magnite.app --stdout F --stderr F`; quit with the Quit Apple Event (the reply may be `-128`).
- Helpers that use `local` in loops must run under `bash`, not the tool's zsh.

## Things learned in Phase 8 that later phases must respect

- `bash packaging/macos/verify_installed.sh <app> <log-dir> build/packaging/macos/test-cache` is the full acceptance set (~2.5 min with a warm
  cache). Phase 10 runs it on the second account (cold cache: expect model downloads, and the ~10 s Metal compile).
- The only failures are ISSUE-010; once it is fixed, rebuild (`build_app.sh` → `build_dmg.sh`) and re-run this script for a clean 11/11.

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

- **Phase 11 (Developer ID signing + notarization) is DEFERRED (was BLOCKED).** Needs: Apple Developer
  Program enrollment, a Developer ID Application certificate in the login keychain, the
  Team ID, and a `notarytool store-credentials` profile. See `adr/0005-signing-approach.md`.
- *(Done 2026-10-28.)* **Phase 10 needed a second macOS user account** on this M1 (the agreed clean-install
  proxy). Its name must be supplied, or the maintainer runs `CLEAN_INSTALL.md` there.
- Open, non-blocking issues: ISSUE-001 (no macOS parent-death signal; verify in Phase 9), ISSUE-004 (disk, 21 GiB free),
  ISSUE-005 (test/`_remove_hf` symlink handling), ISSUE-007 (hub 1.30 leaks disk on delete for shared-blobs caches; decision), ISSUE-008 (unpinned transitive deps; before Phase 7/12),
  ISSUE-009 (Qt license texts; Phase 12),
  **ISSUE-010 (default 4-bit QLoRA fails on macOS; blocks Phase 8)**. ISSUE-006 resolved in Phase 5.

## Standing rules for every session

- **NO GIT COMMITS (maintainer instruction, 2026-10-24).** Keep every change as uncommitted local work. Do not `git commit`, amend, push,
  merge or stash-drop. The phase commits up to `ef173fd` exist only locally and have never been pushed. Builds made from the
  uncommitted tree record `working_tree_modified: true` in `build.json`, which is accurate and expected now.

- One phase per session. Stop at the acceptance check and say it is safe to `/clear` (no commit; see the rule above).
- Update `STATUS.md`, `LOG.md` and `PLAN.md` statuses as work happens, not at the end.
- Shared-file edits (`app/CMakeLists.txt`, `app/src/backend_manager.cpp`,
  `packaging/prepare_runtime.py`) must be additive and platform-guarded, logged in
  `DECISIONS.md`, and followed by re-running the offline regressions **and** the plain
  CMake build, with the result in `LOG.md`.
- Never commit `dist/` artifacts, model weights, build outputs or secrets.
- Never modify the Linux packaging or `dist/` AppImage; they are read-only references.
- Tick only macOS lines in `RELEASE_CHECKLIST.md`, only with logged evidence.
- Nothing is "verified" unless the check ran in that session and is in `LOG.md`.
