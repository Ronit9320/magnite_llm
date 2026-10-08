# Magnite v0.1 testing build

This is an unfinished application packaged for testing, not a stable release.

The package contains Qt, Python 3.12 and the backend with CPU llama.cpp,
PyTorch, LoRA/PEFT and document retrieval dependencies. It does not contain model
weights. Initial model/embedding downloads require internet access. CPU training
is suitable only for small tests. CUDA acceleration and quantized training are
not supported by this CPU package. vLLM and Ollama are not bundled.

## Linux

Make the AppImage executable, then launch it:

```sh
chmod +x Magnite-0.1.0-test.1-x86_64.AppImage
./Magnite-0.1.0-test.1-x86_64.AppImage
```

If FUSE is unavailable, use `--appimage-extract-and-run`, or extract with
`--appimage-extract` and launch `squashfs-root/AppRun`.

To keep test data separate from an existing install:

```sh
MAGNITE_HOME="$HOME/.magnite-testing" \
XDG_CONFIG_HOME="$HOME/.config/magnite-testing" \
./Magnite-0.1.0-test.1-x86_64.AppImage
```

Hugging Face caches are separate; use `HF_HOME` to isolate downloaded weights too.
Without these overrides the package uses the same user data as a source build.
Removing the AppImage does not remove models, adapters or user data.

The initial local build is x86-64, built on Arch Linux with Qt 6.11.2, and requires
**glibc 2.43 or newer**. Older Ubuntu/Debian installations cannot run this build.
AppImage does not bundle glibc. A build on an older distribution is needed for
broader Linux compatibility. See the adjacent verification report for evidence.

## Smoke tests

The application accepts `--theme-check`, `--ui-check` and `--selftest`.
Run tests with a disposable `MAGNITE_HOME`. Model feature tests also accept the
`MAGNITE_SELFTEST_*` flags documented in the repository's AGENTS.md.

`build-info/` inside the package records the source revision, whether the source
tree was modified, and resolved Python dependencies. Python package license
metadata stays alongside installed packages; project/font notices are under
`licenses/`. Public redistribution review remains separate from this test build.
