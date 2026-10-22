# macOS packaging (v0.1 testing build)

Scripts here build a self-contained `Magnite.app` for **Apple Silicon (arm64)**
and wrap it in a testing `.dmg`. They are re-runnable from scratch and do not
touch the Linux packaging, the developer virtualenv or `~/.magnite`.

Nothing is implemented yet — Phase 0 only created this directory. The plan,
current status and evidence live in [`docs/release/macos/`](../../docs/release/macos/):
read `STATUS.md` first, then the current phase of `PLAN.md`.

Planned contents:

| File | Phase | Purpose |
|---|---|---|
| `Info.plist.in` | 2 | bundle identifier, version, `LSMinimumSystemVersion 14.0` |
| `make_icon.sh` | 2 | `app/resources/brand/symbol.svg` → `magnite.icns` |
| `requirements-macos-arm64.txt` | 3 | pinned arm64 runtime dependencies |
| `requirements-macos-quantization.txt` | 3 | optional `bitsandbytes`, kept separate on purpose |
| `prepare_runtime.sh` | 3 | stage the isolated Python runtime with macOS build flags |
| `build_app.sh` | 5 | build → `macdeployqt` → thin → stage runtime → notices → ad-hoc sign |
| `verify_bundle.py` | 5 | reject non-arm64 slices, host paths, wrong `minos`, bad signatures |
| `ml_matrix.py` | 6 | measure CPU/MPS/Metal/quantization support |
| `build_dmg.sh` | 7 | `hdiutil` UDZO image with a drag-to-Applications layout |
| `TESTING.md` | 12 | tester instructions, including the Gatekeeper steps |
| `sign_and_notarize.sh` | 11 | Developer ID signing + notarization (**BLOCKED**, no certificate) |

Requirements on the build host: Command Line Tools, CMake, `uv`, and the pinned
official Qt referenced by `MAGNITE_QT_ROOT` (see
`docs/release/macos/adr/0006-qt-provenance.md`). Builds are **unsigned testing
builds**: ad-hoc signed only, not notarized, and not fit for public distribution.
