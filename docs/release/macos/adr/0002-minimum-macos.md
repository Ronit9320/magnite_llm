# ADR 0002 — Minimum macOS version: 14.0 (Sonoma)

- Date: 2026-10-23
- Status: Accepted (confirmed by the maintainer)

## Context

Three bundled components each impose a floor:

| Component | Floor | Evidence |
|---|---|---|
| Qt 6.8.3 (official binaries) | macOS 12.0 | `otool -l QtCore.framework/QtCore` → `minos 12.0`, `sdk 14.4` |
| python-build-standalone 3.12.12 arm64 | macOS 11.0 | upstream deployment target |
| `torch==2.14.0` | **macOS 14.0** | PyPI wheel tag `macosx_14_0_arm64` |

The build host is macOS 27.0.1 with Command Line Tools 14.3 (clang 14.0.3,
newest available SDK 13.3) and no full Xcode. `CMAKE_OSX_DEPLOYMENT_TARGET` was
empty in the existing cache, so the Oct 4 binary inherited `minos 13.3` from the
SDK — accidental, not chosen.

## Options

1. **14.0** — matches the torch floor; everything else already goes lower.
2. **13.x** — requires pinning a much older torch (the ~2.2 era, tagged
   `macosx_11_0_arm64`), diverging from `packaging/requirements-cpu.txt` and
   giving up tested MPS behaviour.
3. **26/27 (host only)** — honest about what gets tested, but needlessly narrow;
   the binaries would in fact run on 14+.

## Decision

Declare 14.0. Pass `-DCMAKE_OSX_DEPLOYMENT_TARGET=14.0` explicitly and set
`LSMinimumSystemVersion` to `14.0` in `Info.plist`.

## Consequences

- Only macOS 27.0.1 is actually exercised. **14.x and 15.x are declared, not
  verified**, and documentation must say so rather than claiming support.
- Raising the bundled torch version later can only raise this floor; lowering it
  requires revisiting option 2.
- `verify_bundle.py` fails any Mach-O in the bundle whose `minos` exceeds 14.0.
