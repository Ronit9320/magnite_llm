# Changelog

## v0.1-preview — Linux development preview

### Available workflows

- Hugging Face model registry/downloads and optional Ollama integration.
- Local inference through llama.cpp, torch, and optional vLLM.
- LoRA/QLoRA training, durable job history, and adapter import/export/activation.
- Local document ingestion, SQLite/NumPy retrieval, and RAG chat context.
- Studio 03 light/dark interface with reduced motion and keyboard navigation.

### Release preparation

- MIT project license, contributor guidance, issue/PR templates, and draft security policy.
- Public installation, workflows, API, storage/privacy, and troubleshooting documentation.
- Synthetic example data, current preview screenshots, and explicit release limitations.
- CI workflow for base regressions/build/theme/IPC, plus documented checks in a fresh base source/venv installation. Hosted CI execution remains pending.

Preview delivery adds GitHub Actions/AppImage releases, a single version source,
signed metadata, in-app update controls, an idle gate and atomic installation
with startup recovery. Hosted release verification remains pending. See
[verification](docs/release/VERIFICATION.md) and [remaining gates](RELEASE_CHECKLIST.md).
