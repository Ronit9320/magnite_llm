# Installation

The current release is **magnite-spacev0.2 (v0.2-preview)**, a development
preview with packages for Linux, macOS and Windows on the
[releases page](https://github.com/Ronit9320/magnite_llm/releases). Each package
bundles Qt and its own Python runtime and updates itself
from Settings → Updates. Source builds are described below.

## Preview packages

| Platform | Package | First launch |
| --- | --- | --- |
| Linux x86-64, glibc 2.35+ | `Magnite-v0.2-preview-linux-x86_64.AppImage` | `chmod +x` it, keep it in a folder you can write to, and run it. Without FUSE, run it with `--appimage-extract-and-run`. [Tester notes](../packaging/TESTING.md) |
| macOS 14+, Apple silicon | `Magnite-v0.2-preview-arm64.dmg` | Drag it to Applications, then approve it once. [First launch on macOS](MACOS_FIRST_LAUNCH.md) |
| Windows 10 (1809+) / 11, x64 | `Magnite-v0.2-preview-windows-x86_64-setup.exe` | Per-user installer; approve it once. [First launch on Windows](WINDOWS_FIRST_LAUNCH.md) |

Compare each download with the release's `SHA256SUMS` before opening it. The
macOS and Windows packages are not signed with Apple or Microsoft certificates
yet, so both systems warn on first launch; the linked guides explain the steps.
Linux and Windows packages run models on the CPU; the macOS package uses the
Apple GPU (Metal/MPS). Blueprints and Tool mode are not included in v0.2-preview.

## Requirements

- C++20 compiler, CMake 3.20+, and Qt 6 development libraries: Widgets, Network, Svg; OpenSSL Crypto development files.
- Python 3.10+ with venv and pip; native build tools for Python packages.
- Disk space for the Python environment, model/embedding caches, datasets, and adapters.
- Sufficient RAM/VRAM for the specific model, context length, and training configuration.

A clean exported source tree and fresh base-only Python 3.12.12 venv passed the
build, regressions, theme audit, and desktop selftest on the existing Arch Linux
host. See [verification](release/VERIFICATION.md) and the
[observed base package versions](release/base-environment.txt).
There is no universal minimum VRAM guarantee. CPU GGUF inference does not require
training extras; quantized training is platform/hardware dependent.

Install your distribution's C++ toolchain, CMake, Python/venv support, Qt 6 base
and SVG development packages first. Package names vary by distribution. The
release still needs a tested clean-OS recipe; this guide does not claim
that the existing developer environment proves reproducible installation.

## Source build

```bash
git clone https://github.com/Ronit9320/magnite_llm.git
cd magnite_llm
python3 -m venv .venv
source .venv/bin/activate
python -m pip install -e 'backend[ml]'
cmake -S app -B build
cmake --build build -j2
./build/magnite-app
```

Use `python -m pip install -e backend` for the smaller GGUF inference setup.
The base install still includes llama-cpp-python, NumPy and safetensors. `[ml]`
adds torch, transformers, PEFT, bitsandbytes, sentence-transformers, datasets,
accelerate and pypdf. It does not install vLLM or the Ollama application.
Editable source dependencies have broad lower bounds. Packaged runtimes use
platform-specific `requirements-*.txt` and full `constraints-*.txt` locks under
`packaging/`, `packaging/macos/` and `packaging/windows/`; the native integration
jobs installed and tested those locked runtimes. This does not lock an
unconstrained editable install or establish every GPU configuration.

Blueprints and Tool mode are switched off on release branches such as
`release/magnite-spacev0.2` (see `backend/src/backend/_features.py`). On `dev`,
Tool mode in a source checkout needs Node 22.19 or later and
`npm ci --ignore-scripts --prefix backend/src/backend/tooling/pi`; packages
built with the features on include their own Node and Pi.
[Blueprints and Tool mode](BLUEPRINTS.md)

Run from this layout: `<repo>/build/magnite-app` and `<repo>/.venv/bin/python`.
The app looks for `backend/src` relative to the executable. On POSIX it prefers
`.venv/bin/python`, with `python3` as fallback. Windows probes `.venv312` and
`.venv` interpreters before the `python`/`py` launchers; see
[Windows source builds](../BUILD_WINDOWS.md). An arbitrary copied executable is
not a complete installation.

## Engines and first downloads

Use GGUF weights for llama.cpp. Use compatible transformer snapshots for torch
and training. vLLM must be installed into the backend's Python environment with
compatible hardware; it starts a separate local subprocess. Ollama must already
be installed and running; `MAGNITE_OLLAMA_URL` defaults to
`http://127.0.0.1:11434`. Treat these optional integrations as experimental until
live release checks pass.

GPU acceleration for llama.cpp depends on how llama-cpp-python was built. A
working torch CUDA install does not by itself prove llama.cpp GPU offload works.
Use the engine vendors' installation instructions for the actual hardware and
version selected; no new dependency or GPU configuration is prescribed here.

Hugging Face search/download and the first RAG embedding load need network access
unless all required files are cached. Gated repositories require access granted
to your HF account and authentication available to huggingface_hub in the
backend's environment. Do not put tokens in issue reports or screenshots.

## Standalone backend

With the venv activated:

```bash
magnite-backend --port 0 --home /tmp/magnite-demo
```

The startup log prints the chosen loopback port. This runs separately from the
backend started by the desktop; the desktop does not attach to it automatically.
See [local API examples](LOCAL_API.md).

## Updating

Packaged builds update from **Settings → Updates**: check, download, then choose
**Restart to install** when your work is finished. Every update is verified
against a signature built into the app and its SHA-256 before it is installed,
and your data stays where it is.

- **Linux:** the AppImage must be writable, in a writable folder, and not a
  symlink. A version that doesn't start is rolled back.
- **macOS:** open the app from Applications (or another writable folder), not from
  the disk image. A version that doesn't start is rolled back. macOS may ask to
  allow App Management once.
- **Windows:** updates need the copy made by the installer. The new installer runs
  silently over it; a version that installs but doesn't start is reported, not
  rolled back.
- v0.1-preview cannot update to v0.2-preview in place (the data format changed).
  Download the new AppImage; it opens your existing data.

For source builds: stop running jobs and close the app. Back up the application
store and note cache locations first. Reinstall the editable backend and rebuild
the client after pulling a reviewed revision. Database migration/rollback across
releases has not yet been established; retain your backup.
