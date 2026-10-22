# ADR 0001 — Target architecture: Apple Silicon (arm64) only

- Date: 2026-10-23
- Status: Accepted (confirmed by the maintainer)

## Context

The v0.1 macOS package must be self-contained: Qt frameworks, an isolated Python
runtime and the ML dependency set all live inside `Magnite.app`. Qt 6.8.3 as
installed here is universal (`x86_64 arm64`, checked with `lipo -info`), so the
C++ client alone could be universal. The Python half cannot:

- `torch==2.14.0` publishes only `macosx_14_0_arm64` wheels on PyPI (checked
  2026-10-23); there is no x86_64 macOS wheel and no sdist.
- python-build-standalone (what `uv python install` fetches) has no universal2
  macOS build; it ships separate `aarch64` and `x86_64` artifacts.

A universal bundle would therefore need two complete Python runtimes with two
dependency trees, and would still have no x86_64 torch.

## Options

1. **arm64 only.** One runtime, one dependency set. Intel Macs unsupported.
2. **Universal2.** Two runtimes; blocked outright by the missing x86_64 torch wheel.
3. **arm64 now, separate Intel build later.** Needs an Intel Mac or Rosetta build
   host plus an older torch; a different package, not a different slice.

## Decision

Build arm64 only. Thin the universal Qt frameworks to arm64 with `lipo -thin`
during bundle assembly so the shipped `.dmg` carries no dead x86_64 weight.
Document Intel Macs as unsupported for v0.1.

## Consequences

- Smaller artifact; one dependency matrix to verify.
- Intel Macs (and Rosetta) are explicitly out of scope and must not be implied
  as supported in the README, release notes or download page.
- `verify_bundle.py` (Phase 5) treats any non-arm64 Mach-O slice as a failure,
  which also catches a Qt thinning step that silently did not run.
