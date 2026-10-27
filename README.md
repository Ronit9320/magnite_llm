# Magnite LLM

Train adapters, run models, and chat with your documents on your own machine.
Magnite is a native C++20/Qt 6 desktop app backed by a local Python service.
The release scope is local training, inference, and RAG; hosted inference and
cloud training are excluded.

**[v0.1-preview](https://github.com/Ronit9320/magnite_llm/releases/tag/v0.1-preview):**
Linux x86-64 AppImage available, with signed in-app preview updates.
See the [pipeline architecture and setup](docs/development/RELEASE_PIPELINE.md)
and [release checklist](RELEASE_CHECKLIST.md) for verification and deferred work.
Hosted source and CPU AppImage checks passed on Ubuntu. Native desktop coverage
is still limited; macOS and Windows are not yet verified.

Capture the current interface using the [screenshot instructions](docs/release/SCREENSHOTS.md).

## What you can do

- Download and manage Hugging Face models; connect to an existing Ollama service.
- Fine-tune transformer models with LoRA or QLoRA and track training jobs.
- Export, import, and activate trained adapters through the PEFT/torch engine.
- Run local streaming inference with llama.cpp, torch, or an optional vLLM installation.
- Ingest PDF, text, Markdown, and HTML documents, test retrieval, and use RAG in chat.
- Use the Studio 03 interface with light/dark themes and reduced motion.

## Compatibility

| Workflow | Input / dependency | Current status |
| --- | --- | --- |
| GGUF inference | GGUF weights, llama-cpp-python | Tiny-model checks recorded on Linux; hardware acceleration depends on the installed build |
| Transformer inference | Compatible HF/local transformer weights, `[ml]` | torch path exercised with tiny models |
| LoRA training | Transformer base weights and JSONL/CSV/text dataset, `[ml]` | Tiny-model workflow exercised |
| QLoRA training | Supported GPU and bitsandbytes quantization, `[ml]` | Historical tiny-model check; not a promise of support on every device |
| Trained adapter inference | PEFT adapter and matching base model, `[ml]` | Activation restarts torch; clearing unloads the model |
| RAG | sentence-transformers; pypdf for PDF, both in `[ml]` | Local SQLite/NumPy retrieval; first use may download embedding weights |
| vLLM / Ollama | Separately installed engine/service | Integrations exist; live release verification pending |

GGUF weights are for the inference path, not the ordinary PEFT training input.
The local service uses a custom SSE protocol and is not a drop-in OpenAI API.
See [API examples](docs/LOCAL_API.md) and [known limitations](docs/LIMITATIONS.md).
The [backend process map](docs/development/BACKEND_PROCESS.md) traces startup and service calls;
the [backend audit](docs/development/BACKEND_AUDIT.md) records findings, tests, and remaining gaps.

## Build and run

Requires Python 3.10+, CMake 3.20+, a C++20 compiler, and Qt 6 Widgets, Network,
and Svg development packages, plus OpenSSL Crypto development files. Python dependencies may compile native code,
including llama-cpp-python. See [installation details](docs/INSTALL.md).

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

For GGUF inference without training/RAG, install `-e backend` instead. Keep the
binary in `build/` and the virtual environment in `.venv/`: backend discovery
currently depends on this checkout layout. vLLM and Ollama are separate installs.

Start with the [workflow guide](docs/WORKFLOWS.md) and the small
[example inputs](examples/README.md). For failures, use the
[troubleshooting guide](docs/TROUBLESHOOTING.md).

## Architecture and data

```text
Qt desktop: Models | Training | Knowledge | Adapters | Chat | Settings
                         |
                 BackendClient HTTP/SSE
                         |
           FastAPI on 127.0.0.1:<random port>
       /models /datasets /training /adapters /inference /rag /health
                         |
       Local engines, SQLite stores, model and embedding caches
```

The client starts and supervises the backend; the C++ shell never performs ML.
Application data defaults to `~/.magnite` on Linux. `MAGNITE_HOME` overrides it;
model hub caches and Qt settings have separate locations. See
[storage and privacy](docs/STORAGE_AND_PRIVACY.md) before moving or backing up data.

Training, inference, and document processing run locally. Model searches and
weight downloads contact Hugging Face; Ollama uses its configured service URL.
Preview update checks contact GitHub.
There is no application telemetry implementation. Cached weights are needed
for offline work. The backend is intended for local desktop use and currently
has no application authentication layer.

## Development and release

- [Contributing and verification](CONTRIBUTING.md)
- [Current verification report](docs/release/VERIFICATION.md)
- [Release checklist](RELEASE_CHECKLIST.md) and [draft release notes](docs/release/RELEASE_NOTES.md)
- [Current theme rules and branch workflow](CONTRIBUTING.md)
- [Security reporting](SECURITY.md)

The [September audit](docs/development/AUDIT_2026-09-27.md) preserves historical defects and their
subsequent repairs; its follow-up supersedes the original findings. It is not a
promise that every engine or hardware configuration has been tested.

## License

Magnite's original code, documentation, and example inputs are under the
[MIT license](LICENSE). Third-party assets and dependencies retain their own
terms; see [third-party notices](THIRD_PARTY_NOTICES.md). Model and dataset licenses
are separate from the app's license.

## Windows build

See [BUILD_WINDOWS.md](BUILD_WINDOWS.md) for Windows build instructions. The codebase has been prepared for Windows compatibility while maintaining Linux support.
