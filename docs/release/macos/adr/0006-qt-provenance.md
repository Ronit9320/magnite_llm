# ADR 0006 — Qt provenance: pinned official Qt 6.8.3, not Homebrew

- Date: 2026-10-23
- Status: Accepted

## Context

Homebrew's `qt` formula is the convenient choice and the wrong one for a
redistributable package: bottles are built per macOS release and carry a
deployment target equal to the bottle's OS, so an app linked against a bottle
built on the host would effectively require the host's macOS version. Homebrew
Qt would also pull the current 6.10.x, whose build requirements exceed the
Command Line Tools 14.3 / SDK 13.3 available here.

An official Qt installation already exists on this machine and is proven against
this source tree:

- Path: `/Users/architbhuta/magnite_llm-master/.qt-tools/Qt/6.8.3/macos`
- `lipo -info lib/QtCore.framework/QtCore` → `x86_64 arm64`
- `otool -l` → `minos 12.0`, `sdk 14.4`
- `bin/macdeployqt` present
- `build/CMakeCache.txt` in this repo already points `Qt6_DIR` at it, and
  `build/magnite-app` (2026-10-04, arm64) was linked against it

Note that it lives **outside** this repository, in a sibling checkout.

## Options

1. Homebrew `qt` — rejected above.
2. **Reuse the pinned Qt 6.8.3 at the path above**, referenced through an
   environment variable.
3. Re-download Qt into this repo with `aqtinstall` — ~5 GiB duplicated against
   20 GiB of free disk, for no behavioural gain.

## Decision

Use the existing Qt 6.8.3. Every macOS build command takes
`MAGNITE_QT_ROOT=/Users/architbhuta/magnite_llm-master/.qt-tools/Qt/6.8.3/macos`
and passes it as `CMAKE_PREFIX_PATH`; the scripts fail loudly with that variable
unset rather than silently finding another Qt. Record the exact version and
`minos` in `LOG.md` and in the provenance document.

## Consequences

- The build is not reproducible on a machine without that directory. The macOS
  packaging README must state the Qt version, the expected layout and that
  `aqtinstall` can recreate it.
- Qt frameworks arrive universal and are thinned to arm64 with `lipo -thin`
  during assembly (see ADR 0001), so Qt's `minos 12.0` never conflicts with the
  declared 14.0 floor — it is below it.
- `verify_bundle.py` fails if any bundled Mach-O still references the Qt source
  path or any other `/Users/` location.
