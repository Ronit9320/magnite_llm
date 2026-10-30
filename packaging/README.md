# Linux preview packaging

The canonical first release is **v0.1-preview**. Work continues on the original
`release/v0.1-testing` branch using current `dev`; `master` is untouched.
See [the release/update architecture and work log](../docs/development/RELEASE_PIPELINE.md)
for CI, signing setup, updater behavior, deferred checklist work and later platforms.

## Linux x86-64

Requires Python 3.12+, CMake, a C++20 compiler, OpenSSL development files,
Qt6 Widgets/Network/Svg development files, binutils, `curl`, and `uv`.
The preview CI builds on Ubuntu 22.04 and rejects glibc requirements above 2.35.

```sh
bash packaging/build_appimage.sh
```

Artifacts go to `dist/`; staging, logs and checksum-verified deployment tools
stay under `build/packaging/`. The builder downloads isolated Python 3.12.12,
installs the pinned CPU ML runtime and the backend wheel, builds portable CPU
llama.cpp, then assembles Qt and native libraries into an AppImage. No developer
venv, personal store or model caches are copied.

`requirements-cpu.txt` pins direct runtime requirements and `constraints-cpu.txt`
pins their resolved versions. Review both when updating dependencies. AppImage
tool digests and source commits are in `tools.lock.json`; a changed upstream
continuous-channel binary is rejected rather than silently trusted. The build
is not claimed byte-for-byte reproducible: native distribution packages, Python
build dependencies and remote artifact availability still affect reproduction.

Build on the oldest supported Linux environment. AppImage does not bundle glibc.
For a local diagnostic build on a newer host only, set `MAGNITE_GLIBC_MAX` to its
accepted maximum; the compatibility report records the actual ELF requirement.
Do not advertise that artifact as compatible with the older CI baseline.

Version, commit, source dirty state and resolved Python packages are recorded in
`usr/bin/build-info/`. Project/font/Qt notices and Python license metadata are
included. An adjacent packaged runtime is mandatory; packaged startup never
falls back to host Python. Source checkout discovery remains available otherwise.

```sh
python3 packaging/smoke_appimage.py dist/Magnite-v0.1-preview-linux-x86_64.AppImage \
  --logs build/packaging/relocated-tests --cache build/packaging/test-cache --features
```

The smoke test requires loopback access and downloads tiny models. It checks
relocated theme/UI/startup, GGUF inference, and CPU LoRA/adapter/RAG while
supplying deliberately broken host Python paths. CI signs and publishes only
after the exact package passes. Local builds do not publish or push anything.

## macOS Apple silicon (arm64)

The macOS recipe lives in [`macos/`](macos/README.md). It needs Command Line Tools, CMake, `uv` and the pinned official Qt 6.8.3
(`MAGNITE_QT_ROOT`), then:

```sh
bash packaging/macos/build_app.sh     # Qt + isolated Python 3.12 runtime + backend -> ad-hoc signed Magnite.app, verify_bundle.py gate
bash packaging/macos/build_dmg.sh     # -> dist/Magnite-v0.1-preview-arm64.dmg and dist/SHA256SUMS-macos
bash packaging/macos/check_dmg.sh dist/Magnite-v0.1-preview-arm64.dmg
bash packaging/macos/verify_installed.sh <app> <log-dir> <hf-cache>   # full acceptance set on an installed copy
```

The runtime is installed from `macos/requirements-macos-arm64.txt` under the full lock `macos/constraints-macos-arm64.txt`.
llama-cpp-python is built from source with Metal; bitsandbytes is not included. The client launches the bundled interpreter with `-I -B`
and never falls back to a system Python once `Contents/Resources/runtime` exists. Regressions run through
`bash packaging/macos/run_regressions.sh` (a bare `unittest` run is affected by the `/var` symlink).

These are **testing** builds: ad-hoc signed, not notarized, one architecture, minimum macOS 14.0. Developer ID signing and notarization
are not done (blocked on a certificate). Tester instructions: [`macos/TESTING.md`](macos/TESTING.md). Evidence and limits:
[`docs/release/macos/V0_1_MACOS_TESTING.md`](../docs/release/macos/V0_1_MACOS_TESTING.md).

## Native integration automation

See [native package operations](../docs/development/NATIVE_PACKAGES.md) for the
artifact-only three-platform coordinator and [Windows packaging](windows/README.md)
for the new per-user CPU installer. Reusable jobs check the same source SHA,
require clean provenance and upload independent packages/checksums/evidence.
Hosted success and native device gaps are tracked in the multi-platform plan.
