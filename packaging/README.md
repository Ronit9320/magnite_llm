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
