# ADR 0007 — Bundle layout and opt-in CMake bundle target

- Date: 2026-10-23
- Status: Accepted

## Context

`app/CMakeLists.txt` currently produces a plain executable and installs it with
`install(TARGETS magnite-app RUNTIME DESTINATION bin)`. `AGENTS.md`,
`packaging/README.md`, `docs/release/VERIFICATION.md` and CI all invoke
`./build/magnite-app` directly.

Setting `MACOSX_BUNDLE TRUE` unconditionally would change the macOS build output
to `build/magnite-app.app/Contents/MacOS/magnite-app`, quietly breaking every one
of those documented commands on this platform while leaving Linux untouched —
exactly the kind of asymmetric surprise the shared-file rule exists to prevent.

The bundle itself needs a layout decision too. Target:

```
Magnite.app/Contents/
  Info.plist                  LSMinimumSystemVersion 14.0, id com.magnite.magnite-llm
  MacOS/magnite-app           the only executable here
  Frameworks/Qt*.framework    macdeployqt output, thinned to arm64
  PlugIns/                    Qt platform/style/image plugins
  Resources/magnite.icns
  Resources/runtime/python/   python-build-standalone + backend wheel (ADR 0003)
  Resources/build-info/       python-packages.txt, build.json
  Resources/licenses/         LICENSE, THIRD_PARTY_NOTICES.md, font OFL files
```

## Options

1. Unconditional `MACOSX_BUNDLE` — breaks the documented dev commands on macOS.
2. **`option(MAGNITE_MACOS_BUNDLE OFF)`** — bundle only when packaging asks.
3. A separate CMake target for the bundle — duplicates the source list, which
   `AGENTS.md` already requires to be maintained by hand in `MAGNITE_SOURCES`.

## Decision

Option 2. Everything bundle-related sits inside `if(APPLE)` and behind
`MAGNITE_MACOS_BUNDLE`, default `OFF`. `packaging/macos/build_app.sh` passes
`-DMAGNITE_MACOS_BUNDLE=ON`; developers and CI keep getting a plain binary. The
non-bundle `install(TARGETS ... RUNTIME DESTINATION bin)` rule is preserved for
every other configuration, so the Linux AppImage install step is unaffected.

Assembly order is part of this decision and is not negotiable in Phase 5:
`macdeployqt` runs on the **bare** bundle before the Python runtime is staged, so
it never walks ~10k Python files or tries to rewrite torch's dylibs. Thinning,
runtime staging, notices and ad-hoc signing follow, in that order.

## Consequences

- `./build/magnite-app --theme-check` keeps working on macOS; `AGENTS.md` needs
  no correction.
- Two build directories on this machine: `build/` for development (unchanged) and
  `build-macos/` for packaging.
- Anyone who forgets `-DMAGNITE_MACOS_BUNDLE=ON` gets a plain binary and a failed
  install step, which is a loud failure rather than a subtly wrong bundle.
