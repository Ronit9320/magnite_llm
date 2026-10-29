# DECISIONS — macOS v0.1 packaging

ADR index plus a running log of shared-file edits. One ADR per decision in `adr/`.

## ADR index

| ADR | Decision | Alternatives rejected | Why |
|---|---|---|---|
| [0001](adr/0001-target-architecture.md) | **arm64 only** | universal2; arm64 + separate Intel build | `torch==2.14.0` has no x86_64 macOS wheel and python-build-standalone has no universal2 build, so the Python half cannot be universal |
| [0002](adr/0002-minimum-macos.md) | **minimum macOS 14.0** | 13.x with an older torch; host-only 26/27 | the `macosx_14_0_arm64` torch wheel tag is the binding floor; Qt (12.0) and the runtime (11.0) are below it |
| [0003](adr/0003-python-runtime.md) | **python-build-standalone 3.12.12 in `Contents/Resources/runtime`, launched `-I -B`** | `Contents/MacOS/runtime`; embedding libpython | matches the Linux contract; keeps `Contents/MacOS` to one executable; avoids `install_name_tool` invalidating ad-hoc signatures |
| [0004](adr/0004-ml-dependency-strategy.md) | **same pins in a macOS file; llama.cpp Metal via `GGML_METAL_EMBED_LIBRARY`; quantization unsupported until measured** | assume quantization works; install Xcode for the Metal compiler; CPU-only from the start | no `xcrun metal` on this host; bitsandbytes on Apple silicon is unproven; Phase 6 measures every cell |
| [0005](adr/0005-signing-approach.md) | **ad-hoc sign now; Developer ID + notarization deferred (BLOCKED)** | no signature at all; wait for enrollment before building | arm64 requires every Mach-O to be signed; no certificate exists today |
| [0006](adr/0006-qt-provenance.md) | **pinned official Qt 6.8.3 via `MAGNITE_QT_ROOT`** | Homebrew `qt`; re-download with aqtinstall | Homebrew bottles pin the deployment target to the host OS and need a newer toolchain; this Qt is universal, `minos 12.0`, and already proven against this tree |
| [0007](adr/0007-bundle-layout.md) | **`option(MAGNITE_MACOS_BUNDLE OFF)`; macdeployqt before the runtime is staged** | unconditional `MACOSX_BUNDLE`; a second CMake target | keeps `./build/magnite-app` working as `AGENTS.md` documents; stops macdeployqt walking ~10k Python files |
| [0008](adr/0008-merge-mac-branch.md) | **branch from `dev`, merge `mac` in Phase 1** | branch from `mac`; cherry-pick; reimplement | `7fcdb3c` is unmerged and the macOS client cannot reach its backend without it; a merge keeps both histories and stays revertible |
| [0009](adr/0009-local-api-authentication.md) | **Accepted, implemented:** per-launch secret (env) + Host allow-list + data-folder lock for the local API | leave open; separate launchd backend; Unix socket; token file | proven unauthenticated file read and DNS-rebinding exposure (ISSUE-011); smallest change that closes it |

## Shared-file edit log

Every edit to a file shared with Linux/Windows must be additive and
platform-guarded, and must be followed by re-running the offline regressions and
the plain CMake build, with the result recorded in `LOG.md`.

| Date | File | Change | Guard | Reason | Re-verified |
|---|---|---|---|---|---|
| 2026-10-23 | `.gitignore` | appended `build-macos/`, `.venv-macos/`, `*.dmg` | n/a (additive ignore rules) | keep macOS build dirs and artifacts out of commits; `build/` and `dist/` were already ignored | not applicable — no code or build behaviour changes |
| 2026-10-23 | `app/CMakeLists.txt` | added `option(MAGNITE_MACOS_BUNDLE OFF)`; under it: `OUTPUT_NAME Magnite`, `MACOSX_BUNDLE`, Info.plist template, icon, `install(... BUNDLE DESTINATION .)`; the original `install(... RUNTIME DESTINATION bin)` moved into the `else()` branch | `if(APPLE AND MAGNITE_MACOS_BUNDLE)` | ADR 0007: produce `Magnite.app` for packaging without changing `./build/magnite-app` | 53 regressions OK; plain `build/` rebuilt OK; fresh-cache default configure installs `bin/magnite-app`; `--theme-check` 0 failure(s) on both binaries (LOG.md) |
| 2026-10-23 | `packaging/prepare_runtime.py` | new `--requirements` arg (default = the old Linux file); `sys.platform == "darwin"` branch for llama.cpp `CMAKE_ARGS` (Metal embedded, arm64, deployment target 14.0, `MACOSX_DEPLOYMENT_TARGET`), parallelism 4, no `--torch-backend cpu`; tester-doc copy picks `packaging/macos/TESTING.md` on macOS | `sys.platform == "darwin"`; non-darwin branch is the original code verbatim | Phase 3: stage an arm64 runtime with Metal using the same script as Linux | **Proved Linux-identical**: original (`git show HEAD:`) vs patched run under simulated `linux` with `run()` stubbed → identical commands and env (`True`); then 53 regressions OK; plain `build/` rebuilt, `--theme-check` 0 failure(s) |
| 2026-10-23 | `packaging/record_build.py` | `build.json` `runtime` description text differs on darwin (says CPU/MPS torch, Metal llama.cpp, no bitsandbytes) | `sys.platform == "darwin"` | the Linux string "CPU torch and llama.cpp" would be false provenance on macOS | covered by the same run as above |
| 2026-10-23 | `app/src/backend_manager.cpp` | `runtimeDir` probes `appDir/../Resources/runtime` first (cleaned path), falls back to the existing `appDir/runtime`; the Linux `LD_LIBRARY_PATH` hop is skipped on macOS | `#ifdef Q_OS_MAC` / `#ifndef Q_OS_MAC` | ADR 0003 puts the runtime in `Contents/Resources`; without this the bundle falls back to the host `python3` (RED run) | RED→GREEN selftest under a poisoned env; 53 regressions OK; plain `build/` rebuilt, `--theme-check` 0 failure(s); plain dev `--selftest` PASS on the repo `.venv` (LOG.md) |
| 2026-10-23 | `app/CMakeLists.txt` | bundle target gets `INSTALL_RPATH "@executable_path/../Frameworks"` | inside the existing `if(APPLE AND MAGNITE_MACOS_BUNDLE)` | the installed bundle had **no** rpath (install strips the build one) and Qt 6 `macdeployqt` only rewrites existing rpaths, so the app died with `Library not loaded: @rpath/QtWidgets…` | 53 regressions OK; plain `build/` rebuilt, `--theme-check` 0 failure(s); fresh default configure still installs `bin/magnite-app` with no rpath (LOG.md) |
| 2026-10-23 | `packaging/prepare_runtime.py` | darwin branch: `-DLLAMA_OPENSSL=OFF` added to `CMAKE_ARGS`; `CFLAGS`/`CXXFLAGS`/`OBJCFLAGS` get `-ffile-prefix-map=<repo>=.` | `sys.platform == "darwin"` (the `if macos:` block) | llama.cpp linked the host's Homebrew OpenSSL and embedded build-host paths (ISSUE-006 §4–5) | Linux harness re-run against `git show HEAD:` → **`linux: original == patched -> True`**, Linux C/CXX/OBJC flags unset; 53 regressions OK |
| 2026-10-28 | new `backend/src/backend/local_access.py`, `backend/main.py`; `app/src/backend_manager.{h,cpp}`, `backend_client.{h,cpp}`, `mainwindow.{h,cpp}` | **ISSUE-011 / adr/0009:** per-launch secret (env) required on every request except `/health`; Host allow-list; data-folder lock (exit 75 → "already running") | none (security fix for every platform; Windows skips the lock because there is no `fcntl`) | proven unauthenticated file read and DNS-rebinding exposure | `test_local_access.py` (5), 65 OK; dmg `48050836…`: 11/11 acceptance; outside requests 401/400; second instance refused |
| 2026-10-28 | `backend/src/backend/models/registry.py`, `backend/tests/test_cache_discovery.py`, `backend/tests/test_regressions.py` | **ISSUE-005:** `_remove_hf` compares resolved paths; tests resolve their temp root; new symlinked-cache deletion test | none needed (resolving paths is correct on every OS) | deleting a cached model failed when the HF cache path passed through a symlink; plain test runs failed on macOS | new test RED→GREEN by file-copy swap; plain `unittest discover` 60/60 OK |
| 2026-10-28 | `packaging/macos/build_app.sh`, `verify_bundle.py`; new `qt_notices.py`, `packaging/macos/licenses/qt/` | **ISSUE-009:** Qt license texts and the third-party notices for what this Qt build compiles in, copied into `Contents/Resources/licenses/qt/`; the layout check requires them | macOS packaging only | the shipped Qt had no license texts | bundle gate 0 failures; dmg `eecc8ba1…` → 11/11 acceptance |
| 2026-10-24 | `backend/src/backend/main.py` | **ISSUE-001:** `_watch_parent()` thread — polls `os.getppid()` every 1 s; when reparented, SIGTERMs itself (uvicorn graceful shutdown), hard `os._exit(1)` after 15 s | `sys.platform == "darwin"`, and only when the parent is not launchd (pid 1); Linux keeps `prctl(PR_SET_PDEATHSIG)` | macOS has no parent-death signal; Phase 9 measured orphans that accumulate on relaunch | new `backend/tests/test_parent_watch.py` (3 tests incl. a real `kill -9` of a parent on macOS: RED before, GREEN after); 59 regressions OK |
| 2026-10-24 | `backend/src/backend/training/{trainer,jobs}.py`, `routes/training.py` | **ISSUE-010:** `quantization_support()` (False on darwin, or when bitsandbytes is not importable); `/training/start` refuses 4bit/8bit with that reason (HTTP 400) instead of creating a job that fails; new `GET /training/capabilities` | darwin check + package presence; Linux with bitsandbytes unchanged | the default training path failed on macOS | new `backend/tests/test_quantization_support.py` (3 tests); 59 regressions OK |
| 2026-10-24 | `app/src/panels/trainingpanel.{h,cpp}` | **ISSUE-010:** fetches `/training/capabilities`; when unavailable, unticks and disables "Use 4-bit QLoRA" and the precision combo (tooltip = backend reason), overriding a saved 4-bit choice; an older backend without the route keeps every option | capability-driven, not platform `#ifdef` | saved prefs already hold `qlora:true` | plain build OK, `--theme-check` 0, `--ui-check` PASS |
| 2026-10-24 | `app/src/mainwindow.{h,cpp}` | **ISSUE-010:** TRAINING/ADAPTER selftests ask `/training/capabilities` and send `4bit` only where supported (`withSelfTestQuantization`) | capability-driven | keeps Linux QLoRA coverage, fixes macOS | dev-build `MAGNITE_SELFTEST_TRAINING` PASS with `quantization=none` |
| 2026-10-24 | `packaging/requirements-cpu.txt` (**Linux**), `packaging/macos/requirements-macos-arm64.txt` | **ISSUE-007:** `huggingface_hub` 1.30.0 → **1.33.0** | none (both platforms, approved) | 1.30 leaves deleted models' shared blobs on disk (1.83 GB measured) | resolves on both platforms; Linux not installed/run here |
| 2026-10-24 | `packaging/prepare_runtime.py`, new `packaging/lock_requirements.sh`, `packaging/constraints-cpu.txt` (**Linux**), `packaging/macos/constraints-macos-arm64.txt` | **ISSUE-008:** full locks (80 Linux / 79 macOS packages) generated by `uv pip compile`; `prepare_runtime.py` passes `-c constraints-*.txt` (beside `--requirements`, or `--constraints`) | none (both platforms, approved) | transitive deps drifted (certifi) between builds | harness vs `git show HEAD:`: Linux install argv gains only `-c packaging/constraints-cpu.txt`; env and other commands identical. The macOS lock equals the previously shipped set except the hub bump |

Planned (not yet made):

| Phase | File | Change | Guard |
|---|---|---|---|
| 12 | `packaging/README.md`, `README.md`, `RELEASE_CHECKLIST.md` | macOS sections; macOS checklist lines only | n/a |

## Process decisions

- Work only on `release/v0.1-macos`; one phase per session, one commit per phase
  naming the phase. (`AGENTS.md` branch workflow: never develop on `dev`/`master`.)
- The Linux packaging (`packaging/build_appimage.sh`, `packaging/requirements-cpu.txt`,
  `dist/` AppImage, `docs/release/V0_1_LINUX_TESTING.md`) is a **read-only reference**.
  macOS work lives in `packaging/macos/` and `docs/release/macos/`.
- Windows is out of scope. No Windows line in `RELEASE_CHECKLIST.md` is touched;
  shared code stays platform-neutral so Windows work is not blocked later.
- Build steps are scripts under `packaging/macos/`, re-runnable from scratch, not
  ad-hoc terminal commands.
- Functional bugs found along the way go to `ISSUES.md`; they are not silently
  fixed inside packaging work.

## Decisions made during Phase 2 (small; no separate ADR)

- **Executable is `Magnite` inside the bundle.** CMake names `<OUTPUT_NAME>.app`, so `Magnite.app` requires
  `OUTPUT_NAME Magnite`, which also renames the inner executable. Nothing depended on the old name except the plain
  build (CI, AppImage recipe), which is unaffected because the rename applies only in bundle mode. Phases 4/5/8 use
  `Contents/MacOS/Magnite`.
- **`CFBundleShortVersionString` = `0.1.0` (`PROJECT_VERSION`), not the build label.** `0.1.0-test.1` is not a valid
  short-version string (digits and periods only). The label is kept in a custom `MagniteBuildLabel` plist key. This
  deviates from the plan's wording ("short version from MAGNITE_BUILD_LABEL").
- **Icon is generated, not committed.** `make_icon.sh` resolves the `@brandInk`/`@brandOrange` tokens from
  `app/src/theme.h` (the SVG is not valid standalone; rasterized raw it is a solid black blob) and composes a rounded
  paper tile. No colour is duplicated in the script. No cairosvg fallback was written: `qlmanage` works, and an unused
  fallback would be untested code.
- **Phase 2 acceptance corrected** (installed bundle cannot launch before `macdeployqt`); see `PLAN.md`.

## Decisions made during Phase 3 (small; no separate ADR)

- **Printed Linux output preserved.** My first patch changed `'CPU runtime imports OK'`; reverted so only macOS differs
  (`runtime imports OK`), because "Linux behaviour unchanged" includes its log text.
- **`--requirements` is a new argument, not a macOS fork of the script.** One script serves both platforms; the macOS
  wrapper passes `packaging/macos/requirements-macos-arm64.txt`.
- **`bitsandbytes` is excluded from the runtime** (separate `requirements-macos-quantization.txt`), per ADR 0004; confirmed
  not installed. Phase 6 decides whether it may ever be added.
- **Parallelism 4 on macOS** (vs 2 on Linux): the M1 has 8 cores and llama.cpp's compile footprint is small; the build took
  about 2 minutes.
- **The package list is committed** as `docs/release/macos/runtime-packages-macos.txt` (81 packages) — small text provenance.
- **Build/staging stays under `build/`** (gitignored). `scan_macho.py` was promoted to `packaging/macos/` because the plan
  references it.

## Decisions made during Phase 4 (small; no separate ADR)

- **Bundle runtime first, Linux layout second, on macOS only.** If `Contents/Resources/runtime` is missing, the old
  `<MacOS>/runtime` probe still applies, so a non-bundle macOS layout keeps the original behaviour. No new fallback was added: when a
  runtime is found, the "never fall back to a host Python" contract is unchanged.
- **`LD_LIBRARY_PATH` skipped on macOS** rather than renamed to `DYLD_LIBRARY_PATH`: dyld ignores `LD_LIBRARY_PATH`, the wheels
  resolve their dylibs via `@loader_path`/`@rpath` (the GREEN backend started and served `/models`; loading torch/llama.cpp *under the app* is measured in Phase 6/9),
  and `DYLD_*` variables are stripped for hardened-runtime binaries anyway.
- **`backend/src/backend/main.py` not touched** (ISSUE-001 stays with Phase 9, as planned).

## Decisions made during Phase 5 (small; no separate ADR)

- **The runtime is built outside the bundle** (`build/packaging/macos/runtime`, the new default of `prepare_runtime.sh`) and APFS-cloned
  into a bundle that `build_app.sh` rebuilds from scratch every run. The uv-managed source stays a valid, resumable install; all
  bundle-only surgery happens on the clone. (The plan said "reuse the runtime inside the bundle"; that conflicts with rebuilding the
  bundle from scratch.)
- **Finalize is a separate script** (`finalize_bundle.py`: clean → relink → thin → sign), so `build_app.sh` stays a readable recipe
  and the signing order is enforced in one place.
- **Host-path check = this host's paths, in every file (text and binary).** The plan's "any text file containing `/Users/`" cannot be
  zero-tolerance: 11 legitimate files contain `/Users/` as examples (`ntpath.py`, SBOMs, pip). The gate instead fails on the build
  user's home, the checkout and `MAGNITE_QT_ROOT` anywhere — stricter where it matters (it found the binary leaks).
- **Added gate checks beyond the plan:** absolute `LC_RPATH`s, dependency resolution inside the bundle, absolute/escaping/dangling
  symlinks, and a `layout` check that includes the `offscreen` plugin and `qt.conf`.
- **`libqoffscreen` is deployed explicitly** (macdeployqt ships only `cocoa`), as the Linux recipe does — `--selftest` and headless
  checks need it.
- **Absolute rpaths are deleted, not rewritten**, and only after proving nothing needs them; any file that would lose a dependency
  aborts the build instead.
- **llama-cpp-python's root `lib/` and `include/` are removed** from the bundle (development install; `llama_cpp` loads from
  `llama_cpp/lib`). Its `RECORD` is left as is (stale entries are harmless; nothing uninstalls inside the bundle).
- **`prepare_runtime.sh --fresh` also runs `uv cache clean llama-cpp-python`**: uv reuses a built wheel whatever `CMAKE_ARGS` say, so
  without this a flag change is silently ignored.
- **No hardened runtime / entitlements yet** — ad-hoc only (ADR 0005); Phase 11 adds `--options runtime` with Developer ID.
- **`smoke_bundle.sh` promoted to `packaging/macos/`** (the Phase 4 harness was throwaway): Phases 7–10 run the same checks on the
  `.dmg` copy, `/Applications` and the second account.

## Decisions made during Phase 7 (small; no separate ADR)

- **`hdiutil` kept despite the macOS 27 deprecation warning** (`use 'diskutil image create from' instead`). `hdiutil` still works,
  verifies, and exists on every macOS a build host might run. `diskutil image` is new, so using it would tie packaging to a recent
  host. Revisit when `hdiutil create` stops working.
- **UDZO + HFS+, as planned.** 1.1 GB bundle → 320 MB image. ULFO/APFS were not tried: no measured need, and UDZO is the most
  widely readable.
- **No custom Finder window layout** (background image, icon positions). That needs scripting Finder against a read-write image, which
  is fragile in automation. The volume shows `Magnite.app` next to an `Applications` link, which is the drag-to-install layout.
- **`dist/SHA256SUMS-macos`** keeps the macOS checksum separate from the Linux `dist/SHA256SUMS` (rewritten by `build_appimage.sh`).
- **`build_dmg.sh` refuses to package a bundle that fails `verify_bundle.py`** and warns when `build.json` says the tree was modified.
- **No `TESTING.md` on the volume yet** (Phase 12 writes it); the script picks it up automatically once it exists.
- **`check_dmg.sh` smoke-tests the app *inside* the read-only mounted volume.** That also proves the app never writes into its bundle.

## Decisions made while fixing ISSUE-001/007/008/010 (2026-10-24, maintainer: "fix all")

- **No git commits** from here on (maintainer instruction). The fixes are uncommitted local changes, and builds record `working_tree_modified: true`.
- **ISSUE-010 is capability-driven, not a macOS `#ifdef` in the UI.** The backend owns the decision, so the UI, the selftests and any other
  client agree, and a Linux runtime without bitsandbytes is handled too. The backend refuses up front (400) instead of failing a queued job.
- **macOS reports quantization unsupported even if bitsandbytes is installed** (the dev venv has it), because the ML matrix measured it as
  unsuitable. Revisit if a future bitsandbytes runs on MPS.
- **ISSUE-001 is a poll, not kqueue:** a 1 s poll is simple, testable, and the latency does not matter. Graceful SIGTERM first, so
  uvicorn shuts down cleanly; hard exit after 15 s (the same grace the client gives in `BackendManager::stop`).
- **`backend/pyproject.toml` floors unchanged** (`huggingface_hub>=0.24.0`): dev installs stay flexible, and the shipped runtimes are pinned by
  the requirements and locks.

## Decisions made during the 2026-10-28 fixes (ISSUE-005, ISSUE-009)

- **Qt notices are generated from Qt's metadata, not hand-written**, and they are filtered by this build's feature flags. Every component must be
  explicitly included or excluded with a reason, so an upgrade cannot silently add an unlisted component.
- **Over- versus under-inclusion:** when Qt's metadata and the flags say a component is compiled in, it is included even if its use is narrow
  (for example pixman's NEON helpers). Missing a notice is worse than listing an extra one.
- **The notices are fetched with `curl`, not Python's `urllib`:** the python.org build on this host has no CA bundle, and `curl` uses the macOS trust store.
- **Process slip, recorded:** a test command of mine included `git stash push backend/src/backend/models/registry.py`, against the no-git
  instruction. Nothing was committed or pushed; `git stash pop` restored the file and removed the entry (0 stash entries, HEAD still `ef173fd`).
  Before/after experiments now use plain file copies.
