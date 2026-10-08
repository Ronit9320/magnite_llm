# v0.1 Linux test AppImage — 2026-10-08

This is a local test artifact, not a public/stable release. Work is on
`release/v0.1-testing`, branched from `dev` at `5ce5b37`. No tag, public release
or remote publication is part of this build. Windows packaging is deferred at
the user's request until after Linux testing.

## Artifact and environment

- `dist/Magnite-0.1.0-test.1-x86_64.AppImage`, approximately 418 MiB.
- `dist/SHA256SUMS` contains the final artifact checksum.
- Arch Linux x86-64, GCC 16.2.1, Qt 6.11.2, host glibc 2.44.
- Highest referenced GLIBC symbol version in the client, bundled system/Qt
  libraries and locally built llama.cpp libraries: **2.43**. This is not an
  older-Ubuntu/Debian-compatible build.
- Bundled standalone Python 3.12.12, CPU torch 2.14.0 and llama-cpp-python 0.3.35,
  plus the ML extras. Resolved dependencies and source commit/dirty state are
  recorded in `usr/bin/build-info/` inside the image.
- Model/embedding weights, caches, application data, CUDA, vLLM and Ollama are
  not included. CPU LoRA and RAG were tested; QLoRA/GPU paths are outside this
  package's verified scope.

## Changes

The client launches the adjacent bundled interpreter with `-I -B`, ignoring host
Python configuration and using the bundled native libraries. An incomplete
packaged runtime never falls back to a system Python. Development checkout
discovery remains available when no packaged `runtime/` directory is present.
CMake has an install target and build-version label. Packaging scripts stage
the runtime from standalone Python, build the backend wheel, collect Qt and
C++ dependencies, and produce the AppImage and checksum.

## Verification

- Release CMake build passed.
- All 41 backend regression tests passed.
- Theme audit: 0 failures.
- UI interaction checks: PASS, 0 failures.
- AppImage copied to a temporary location outside the checkout, extracted and
  launched with deliberately invalid host `PYTHONHOME` and `PYTHONPATH`.
- Relocated desktop/backend selftest: PASS, 0 failures.
- Relocated GGUF test: load → streamed chat (8 deltas) → unload passed.
- Relocated bundled Python: registered transformer model → chat → 3-step CPU
  LoRA → registered adapter → real document retrieval → adapter/RAG chat →
  export/delete/import → backend restart/history checks all passed. Tiny-model
  token delivery was checked; generated answer quality was not assessed.
- Actual AppImage `--appimage-extract-and-run --theme-check` passed.

Initial sandboxed UI/backend checks could not use the required local services;
the recorded passing runs had loopback access. Feature checks used a dedicated
model cache under `build/packaging/test-cache` and disposable application data.
Logs are retained locally under `build/packaging/` and copied beside the artifact
under `dist/verification/`. The relocation test is reproducible with
`packaging/smoke_appimage.py`.

No clean second machine, older distro, native Wayland/X11 interactive session,
GPU workload, Windows build, or public redistribution certification is claimed.
Use [tester instructions](../../packaging/TESTING.md) to isolate user data.
