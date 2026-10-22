# STATUS — macOS v0.1 packaging

**Read this file first, then the current phase section of `PLAN.md`. Execute only
that phase, then stop.** Do not re-read `RELEASE_CHECKLIST.md` or other long docs
unless the current phase names a specific section.

- Last updated: 2026-10-23 03:44 IST
- Branch: `release/v0.1-macos` (branched from `dev` at `0fa53e3`)
- Current phase: **Phase 1 — Environment audit, pins, and macOS baseline** (TODO)
- Last completed: **Phase 0 — plan, 8 ADRs and state files committed**
- Suggested model for the next phase: **Sonnet** (mechanical)

## Next exact commands

```sh
cd /Users/architbhuta/magnite_llm
export MAGNITE_QT_ROOT=/Users/architbhuta/magnite_llm-master/.qt-tools/Qt/6.8.3/macos
git merge --no-ff mac -m "merge: macOS loopback health probe (7fcdb3c) for v0.1 macOS packaging"
sw_vers; uname -m; clang --version | head -2; cmake --version | head -1; uv --version
"$MAGNITE_QT_ROOT/bin/qmake6" -query QT_VERSION
uv venv --python 3.12 .venv-macos && uv pip install --python .venv-macos -e "backend"
PYTHONPATH=backend/src .venv-macos/bin/python -m unittest discover -s backend/tests -v
cmake -S app -B build-macos -DCMAKE_PREFIX_PATH="$MAGNITE_QT_ROOT" \
      -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
      -DCMAKE_OSX_DEPLOYMENT_TARGET=14.0 -DMAGNITE_BUILD_LABEL=0.1.0-test.1
cmake --build build-macos -j4
./build-macos/magnite-app --theme-check
otool -l build-macos/magnite-app | grep -A3 LC_BUILD_VERSION
```

Then: record versions in `LOG.md` and `environment-macos.txt`, flip Phase 1 to
DONE in `PLAN.md`, rewrite this file, and commit with a message naming Phase 1.

## Phase 1 acceptance

- All offline regressions pass (record the count).
- `--theme-check` prints `0 failure(s)`.
- `LC_BUILD_VERSION` shows `minos 14.0`.
- `build-macos/` is gitignored or added to `.gitignore`.

## Known blockers

- **Phase 11 (Developer ID signing + notarization) is BLOCKED.** Needs: Apple
  Developer Program enrollment, a Developer ID Application certificate in the
  login keychain, the Team ID, and a `notarytool store-credentials` profile. See
  `adr/0005-signing-approach.md`. Nothing else in the plan is blocked by it.
- **Phase 10 needs a second macOS user account** on this M1 (the agreed clean-install
  proxy). Its name must be supplied, or the maintainer runs
  `CLEAN_INSTALL.md` there personally.
- **R8 / ISSUE-001**: no parent-death signal on macOS, so a force-killed GUI likely
  orphans the backend. To be *verified* in Phase 9 and reported, **not** silently
  fixed. See `ISSUES.md`.

## Standing rules for every session

- One phase per session. Stop at the acceptance check and say it is safe to `/clear`.
- Update `STATUS.md`, `LOG.md` and `PLAN.md` statuses as work happens, not at the end.
- Shared-file edits (`app/CMakeLists.txt`, `app/src/backend_manager.cpp`,
  `packaging/prepare_runtime.py`) must be additive and platform-guarded, logged in
  `DECISIONS.md`, and followed by re-running the offline regressions **and** the
  plain CMake build, with the result in `LOG.md`.
- Never commit `dist/` artifacts, model weights, build outputs or secrets.
- Never modify the Linux packaging or `dist/` AppImage; they are read-only references.
- Tick only macOS lines in `RELEASE_CHECKLIST.md`, only with logged evidence.
- Nothing is "verified" unless the check ran in that session and is in `LOG.md`.
