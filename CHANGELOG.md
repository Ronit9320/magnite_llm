# Changelog

## Unreleased — integrated native testing candidates

- Native Linux CPU AppImage, Mac ARM DMG and Windows x86-64 CPU installer
  candidates passed the 2026-10-30 hosted integration workflow from clean source.
  These do not replace the public Linux v0.1-preview assets.
- Per-launch desktop API Bearer credentials, loopback Host checks, and POSIX/Windows
  data-directory locks; platform backend supervision and graceful Windows shutdown.
- Full platform Python dependency constraints and clean-source package provenance.
- Training snapshot/commit serialization and native SQLite/archive lifetime fixes.
- Current package, authentication, storage, and lifecycle documentation refreshed.

See [native package evidence](docs/development/NATIVE_PACKAGES.md). Mac notarization,
Windows signing, platform updater delivery and broader physical-machine/GPU
acceptance remain separate work.

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
- CI workflow for base regressions/build/theme/IPC, plus documented checks in a fresh base source/venv installation. Hosted source and Linux CPU package checks passed on 2026-10-23.

Preview delivery adds GitHub Actions/AppImage releases, a single version source,
signed metadata, in-app update controls, an idle gate and atomic installation
with startup recovery. The first GitHub prerelease was published after hosted
verification passed. See
[verification](docs/release/VERIFICATION.md) and [remaining gates](RELEASE_CHECKLIST.md).
