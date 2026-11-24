# magnite-spacev0.2 — Magnite Space v0.2-preview

A development preview of Magnite Space for **Linux, macOS and Windows**. Download
models, chat with them, ground answers in your own documents and train LoRA
adapters, all on your own computer. It is unfinished software meant for testing:
expect rough edges and keep backups of anything you care about.

## Downloads

| Platform | File | Notes |
| --- | --- | --- |
| Linux x86-64 | `Magnite-v0.2-preview-linux-x86_64.AppImage` | glibc 2.35 or newer (Ubuntu 22.04 and later). Make it executable and run it. |
| macOS, Apple silicon | `Magnite-v0.2-preview-arm64.dmg` | macOS 14 or newer. Not notarized: see [first launch on macOS](https://github.com/Ronit9320/magnite_llm/blob/release/magnite-spacev0.2/docs/MACOS_FIRST_LAUNCH.md). |
| Windows 10/11 x64 | `Magnite-v0.2-preview-windows-x86_64-setup.exe` | Per-user install, no administrator needed. Not code-signed: see [first launch on Windows](https://github.com/Ronit9320/magnite_llm/blob/release/magnite-spacev0.2/docs/WINDOWS_FIRST_LAUNCH.md). |

`SHA256SUMS` lists every file's checksum. Compare it before you open a download.
The `update-*.json` files and their `.sig` signatures are for the in-app updater;
you don't need to download them.

The Linux and Windows packages run models on the CPU. The macOS package uses the
Apple GPU through Metal (chat) and MPS (training). Each bundles its own Python
runtime; model weights are downloaded separately from Hugging Face.

## Warnings on first launch

The macOS and Windows builds aren't signed with a paid Apple or Microsoft
certificate yet, so both systems warn you the first time:

- **macOS:** "Apple could not verify Magnite is free of malware". Open it once
  from **System Settings → Privacy & Security → Open Anyway**.
- **Windows:** "Windows protected your PC". Choose **More info → Run anyway**.
  If Smart App Control is on, Windows blocks the installer outright; this preview
  can't be installed on that PC.

Only approve a copy whose checksum matches `SHA256SUMS`. Updates installed from
inside the app don't show these warnings again.

## Updates

Settings → Updates checks GitHub for newer previews (automatically at startup
unless you turn that off). Each update is checked against a signature built
into the app and its SHA-256 before anything is installed, and installs only when
you choose **Restart to install**. Your models, datasets, adapters and chats are
not touched.

- **Linux:** the AppImage replaces itself and rolls back if the new version does
  not start. Keep the AppImage in a folder you can write to.
- **macOS:** the app replaces its bundle and rolls back if the new version does
  not start. Keep it in Applications (or another folder you can write to), and
  open it from there rather than from the disk image. macOS may ask once to allow
  Magnite Space to update apps.
- **Windows:** the app runs the new installer silently over the current
  installation. If the new version installs but does not start, reinstall the
  previous one from its release page.

**Upgrading from v0.1-preview (Linux):** v0.2 stores data in a newer format, so
v0.1-preview can't update to it from inside the app. Download the new AppImage
and run it; it opens your existing `~/.magnite` data.

## What's new since v0.1-preview

- **Three platforms.** Native macOS (Apple silicon) and Windows packages join the
  Linux AppImage, all built and tested from one source commit.
- **Conversations.** Markdown replies, copy and retry, editing sent prompts with
  version switching, collapsible model thinking, a context-window meter with
  automatic compaction of long chats, and pinned chats, projects and recents.
- **Knowledge (RAG).** Lives inside Conversation. Hybrid keyword + vector search,
  sentence-aware chunking, a choice of embedding models with reindexing, and
  cancellable ingest with progress for large documents.
- **Training Studio and Observatory.** Live training charts, run review and
  comparison, prompt/response datasets, and training in a separate process so the
  app stays responsive.
- **Model hub and Library.** Redesigned search with paging and download sizes,
  cancellable downloads with progress, and an organized library of models,
  adapters and projects.
- **Docs.** An offline reference inside the app covering the workspace, model
  concepts, data quality and training results.
- **Settings window** with diagnostics, storage and privacy controls, runtime
  package installs and update controls.
- **Reliability.** The backend restarts after a crash, damaged chats are reported
  instead of lost, and the local API requires a per-launch credential.

## Not in this release

Blueprints (saved workflows) and Tool mode (letting a model propose actions on
your computer) are still in development and are left out of this build. They'll
come in a later, more stable release.

## Known limitations

- The Linux and Windows packages are CPU builds without CUDA, so QLoRA (which
  needs a supported NVIDIA GPU) is not usable with them; QLoRA is not available
  on macOS. macOS GPU use was checked on an M1 with small models; CI checks the
  macOS package on the CPU.
- vLLM and Ollama are not bundled. An Ollama service you already run can be
  connected.
- Windows has no automatic rollback if an updated version fails to start.
- See [current limitations](https://github.com/Ronit9320/magnite_llm/blob/release/magnite-spacev0.2/docs/LIMITATIONS.md)
  and [troubleshooting](https://github.com/Ronit9320/magnite_llm/blob/release/magnite-spacev0.2/docs/TROUBLESHOOTING.md).

Report problems on [GitHub issues](https://github.com/Ronit9320/magnite_llm/issues)
and sensitive ones through the [security policy](https://github.com/Ronit9320/magnite_llm/blob/release/magnite-spacev0.2/SECURITY.md).
