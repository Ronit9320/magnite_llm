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
