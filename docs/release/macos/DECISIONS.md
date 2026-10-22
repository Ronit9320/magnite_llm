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

Planned (not yet made):

| Phase | File | Change | Guard |
|---|---|---|---|
| 2 | `app/CMakeLists.txt` | bundle target, `Info.plist`, `.icns`, `BUNDLE DESTINATION` | `if(APPLE)` + `option(MAGNITE_MACOS_BUNDLE OFF)` |
| 3 | `packaging/prepare_runtime.py` | macOS `CMAKE_ARGS`; drop `--torch-backend cpu` | `sys.platform == "darwin"` |
| 4 | `app/src/backend_manager.cpp` | probe `Contents/Resources/runtime`; skip the `LD_LIBRARY_PATH` hop | `#ifdef Q_OS_MAC` |
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
