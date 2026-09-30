# Installation from source

The current layout is a developer/source installation, not a relocatable desktop
bundle. No released installer or tested cross-platform matrix is claimed yet.

## Requirements

- C++20 compiler, CMake 3.20+, and Qt 6 development libraries: Widgets, Network, Svg.
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
The base install still includes llama-cpp-python and NumPy. `[ml]` adds torch,
transformers, PEFT, bitsandbytes, sentence-transformers, datasets, accelerate,
safetensors, and pypdf. It does not install vLLM or the Ollama application.
Dependencies currently have broad lower bounds rather than a tested lockfile.

Run from this layout: `<repo>/build/magnite-app` and `<repo>/.venv/bin/python`.
The app looks for `backend/src` relative to the executable and falls back to
`python3` if the repository venv is absent. An arbitrary copied executable is
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

Stop running jobs and close the app before updating. Back up the application
store and note cache locations first. Reinstall the editable backend and rebuild
the client after pulling a reviewed revision. Database migration/rollback across
future releases has not yet been established; retain your backup.
