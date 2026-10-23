# macOS packaging (v0.1 testing build)

Scripts here build a self-contained `Magnite.app` for **Apple Silicon (arm64)**
and wrap it in a testing `.dmg`. They are re-runnable from scratch and do not
touch the Linux packaging, the developer virtualenv or `~/.magnite`.

Done so far: `run_regressions.sh` (1), `make_icon.sh`, `Info.plist.in` (2), the requirements files, `prepare_runtime.sh`, `scan_macho.py` (3), `build_app.sh`, `finalize_bundle.py`, `verify_bundle.py`, `smoke_bundle.sh` (5), `ml_matrix.py`, `probe_hf_cache.py` (6), `build_dmg.sh`, `check_dmg.sh` (7), `verify_installed.sh` (8); the rest are added phase by phase.

Build and check a bundle (about 1–3 minutes; `--fresh-runtime` rebuilds Python deps and llama.cpp):

```sh
export MAGNITE_QT_ROOT=/path/to/Qt/6.8.3/macos
bash packaging/macos/build_app.sh            # ends with verify_bundle.py: "0 failure(s)"
ditto build/packaging/macos/stage/Magnite.app "$TMPDIR/try/Magnite.app"
bash packaging/macos/smoke_bundle.sh "$TMPDIR/try/Magnite.app"
bash packaging/macos/build_dmg.sh             # -> dist/Magnite-0.1.0-test.1-arm64.dmg
bash packaging/macos/check_dmg.sh dist/Magnite-0.1.0-test.1-arm64.dmg
```

The plan, current status and evidence live in [`docs/release/macos/`](../../docs/release/macos/):
read `STATUS.md` first, then the current phase of `PLAN.md`.

Planned contents:

| File | Phase | Purpose |
|---|---|---|
| `run_regressions.sh` | 1 ✔ | offline backend tests with a symlink-resolved `TMPDIR` (macOS `/var` quirk, ISSUE-005) |
| `Info.plist.in` | 2 ✔ | bundle identifier, version, `LSMinimumSystemVersion 14.0` |
| `make_icon.sh` | 2 ✔ | resolves the `@brand*` tokens from `theme.h`, `symbol.svg` → rounded tile → `magnite.icns` |
| `requirements-macos-arm64.txt` | 3 ✔ | pinned arm64 runtime dependencies |
| `requirements-macos-quantization.txt` | 3 ✔ | optional `bitsandbytes`, kept separate on purpose |
| `prepare_runtime.sh` | 3 ✔ | build the isolated Python runtime (in `build/packaging/macos/runtime`) with macOS build flags |
| `build_app.sh` | 5 ✔ | build → `macdeployqt` (+ `offscreen`) → clone runtime → finalize → verify |
| `finalize_bundle.py` | 5 ✔ | clean runtime, fix install ids/rpaths, thin to arm64, ad-hoc sign (in that order) |
| `scan_macho.py` | 3 ✔ | prototype scan: minos and architectures of every Mach-O (feeds `verify_bundle.py`) |
| `verify_bundle.py` | 5 ✔ | reject non-arm64 slices, `minos` > 14.0, absolute links/rpaths, unresolved deps, host paths, bad symlinks, bad signatures |
| `smoke_bundle.sh` | 5 ✔ | run a bundle anywhere with a poisoned host env: theme-check, selftest, interpreter, orphans, signature before/after |
| `ml_matrix.py` | 6 ✔ | measure llama.cpp Metal/CPU, torch CPU/MPS, LoRA/QLoRA, embeddings through the backend's own code → `ML_MATRIX.md` |
| `probe_hf_cache.py` | 6 ✔ | ISSUE-007: backend HF-cache scan/delete on a clone of a shared-blobs cache |
| `build_dmg.sh` | 7 ✔ | gate on `verify_bundle.py`, then an `hdiutil` UDZO image (`Magnite.app` + `Applications` link) and `dist/SHA256SUMS-macos` |
| `verify_installed.sh` | 8 ✔ | full acceptance set on an installed app: theme/ui checks, 6 selftests, `live_workflow.py`, screenshots, signature |
| `check_dmg.sh` | 7 ✔ | mount read-only, check layout, run `smoke_bundle.sh` on the app inside the volume, detach |
| `TESTING.md` | 12 | tester instructions, including the Gatekeeper steps |
| `sign_and_notarize.sh` | 11 | Developer ID signing + notarization (**BLOCKED**, no certificate) |

Requirements on the build host: Command Line Tools, CMake, `uv`, and the pinned
official Qt referenced by `MAGNITE_QT_ROOT` (see
`docs/release/macos/adr/0006-qt-provenance.md`). Builds are **unsigned testing
builds**: ad-hoc signed only, not notarized, and not fit for public distribution.
