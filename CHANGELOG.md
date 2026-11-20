# Changelog

## v0.2-preview — magnite-spacev0.2

First preview for Linux, macOS (Apple silicon) and Windows. See the
[release notes](docs/release/RELEASE_NOTES.md) for the full list.

### Added

- Native macOS DMG and Windows per-user installer alongside the Linux AppImage,
  all built and tested from one commit and published together.
- In-app updates on macOS (bundle swap with rollback) and Windows (silent
  installer run), using the same signed update metadata as Linux.
- Conversation: Markdown replies, copy/retry, edit-and-resend with versions,
  collapsible thinking, a context meter with automatic compaction, pinned chats,
  projects and recents.
- Knowledge inside Conversation: hybrid keyword + vector search, sentence-aware
  chunking, embedding model choice with reindexing, cancellable ingest.
- Training Studio and Observatory, prompt/response datasets, training in a child
  process.
- Redesigned Model hub and Library; offline Docs reference; Settings window with
  diagnostics, storage controls and runtime package installs.
- Per-launch API credentials, loopback Host checks, data-directory locks and
  backend crash recovery.

### Release process

- Releases are published only from `release/**` branches by the manual
  `release.yml` workflow, which builds all three platforms from one commit and
  refuses reused versions or update sequences. `dev` and `master` keep their
  automatic build-and-test runs and never publish.
- A build profile (`backend/src/backend/_features.py`) switches Blueprints, Tool
  mode and the bundled Node.js/Pi off for this release; they stay on in `dev`.

### Changed

- `DATA_SCHEMA` 2: v0.1-preview cannot update in place; install v0.2-preview manually.

### Known gaps

- macOS build not notarized, Windows installer not code-signed.
- No automatic rollback for a Windows update that installs but fails to start.
- Linux and Windows packages are CPU-only; macOS uses Metal/MPS.

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
