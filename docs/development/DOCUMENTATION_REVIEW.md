# Documentation consistency review — 2026-11-02

Reviewed integrated source at `92d01b0` and its recorded release evidence.
Changes in this review are documentation-only; no application behavior, release
identity, package, signing key, public release, or remote branch was changed.

## Current status and evidence

| Topic | Current description | Authoritative implementation / evidence |
| --- | --- | --- |
| Public distribution | Linux x86-64 `v0.1-preview` is published; Mac/Windows candidates remain testing artifacts | [README](../../README.md), [native packages](NATIVE_PACKAGES.md) |
| Integrated packages | Linux AppImage, Mac ARM DMG and Windows CPU installer passed the recorded 2026-10-30 workflow from clean `3ee0a4c` | [Native verification and run link](NATIVE_PACKAGES.md) |
| Linux compatibility | Ubuntu 22.04 build baseline, glibc ceiling 2.35; the Arch-built 2.43 artifact is historical | [Linux workflow](../../.github/workflows/build-linux.yml), [packaging guide](../../packaging/README.md) |
| Desktop API authentication | Fresh Bearer token per launch; all HTTP paths except `/health` require it; loopback Host allow-list | [Access wrapper](../../backend/src/backend/local_access.py), [desktop launch](../../app/src/backend_manager.cpp), [client](../../app/src/backend_client.cpp) |
| Developer API mode | CLI without a token omits credential checks; direct Uvicorn bypasses the CLI wrapper/lock | [Backend entry point](../../backend/src/backend/main.py), [API guide](../LOCAL_API.md) |
| Data ownership | POSIX flock / Windows CRT lock prevents two CLI backends sharing a data directory | [Lock implementation](../../backend/src/backend/local_access.py), [regressions](../../backend/tests/test_local_access.py) |
| Runtime/dependencies | Isolated packaged Python and full platform constraints; editable source bounds remain broad | [Runtime builder](../../packaging/prepare_runtime.py), [Windows builder](../../packaging/windows/build_package.ps1), [installation](../INSTALL.md) |
| Backend lifetime | Linux parent-death signal, Mac parent watcher, Windows kill-on-close Job Object and authenticated graceful shutdown | [Backend main](../../backend/src/backend/main.py), [desktop manager](../../app/src/backend_manager.cpp) |
| Training ownership/history | Active dataset references guarded; output paths keyed by job ID; snapshot/commit serialization | [Job manager](../../backend/src/backend/training/jobs.py), [trainer](../../backend/src/backend/training/trainer.py), [dataset routes](../../backend/src/backend/routes/training.py) |
| Adapter integrity | Validated safetensors imports/exports and independent archive URLs; activation reloads torch/PEFT | [Adapter routes](../../backend/src/backend/routes/adapters.py), [API guide](../LOCAL_API.md) |
| RAG extraction | Inline HTML/Markdown uses common extraction logic; PDFs need paths; no OCR | [RAG routes](../../backend/src/backend/routes/rag.py), [workflows](../WORKFLOWS.md) |
| Storage/privacy | Windows APPDATA path; no telemetry or encryption-at-rest; GitHub/Hub/Ollama network activity documented | [Paths](../../backend/src/backend/paths.py), [storage guide](../STORAGE_AND_PRIVACY.md) |

## What was corrected

- Replaced the obsolete no-package/no-authentication/unlocked-dependencies
  statements in the limitations and installation guides.
- Updated privacy, first-launch, troubleshooting and API guides for current
  platform evidence, credentials, locks, network activity and lifecycle endpoints.
- Refreshed architecture/process/frontend descriptions, including stale adapter
  and RAG endpoint names, raw-socket readiness/version checks, snapshot resolution,
  safe adapter paths, and guarded dataset deletion.
- Updated the release checklist to distinguish completed implementation and
  bounded hosted verification from broader acceptance/publication gates.
- Added an unreleased changelog entry and current navigation links.
- Marked older audits, release notes, Mac phase records, integration inventory
  and Linux handoff as historical rather than rewriting their past test outcomes.
- Added notices to the two local-only release-readiness assessments when present;
  these files remain ignored and are not required by public documentation.

## Remaining limitations are deliberate

This pass does not close Mac notarization, Windows Authenticode signing,
Mac/Windows installed-app updates, complete public platform rollout, broader
clean consumer-machine/physical-GPU acceptance, live vLLM/Ollama release checks,
large-model/OOM and resource-contention coverage, durable chat, automatic training
resume, automatic schema/path migration, PDF OCR, or retrieval-scale measurement.
The security contact remains an owner-deferred placeholder. See
[current limitations](../LIMITATIONS.md) and [release gates](../../RELEASE_CHECKLIST.md).

Historical reports retain source-specific defect descriptions, old package names,
test counts and temporary evidence paths. Their new notices identify the current
references. Historical test passes are not fresh verification of this checkout;
CI configuration alone is not a passing run. The hosted results above were read
from committed evidence, not independently rerun on all three platforms here.

## Validation

- Reviewed endpoint names, dependency declarations and lifecycle behavior against
  the implementation files linked above.
- Checked relative Markdown file targets in all changed tracked guides and this
  report: no missing targets. Historical temporary artifact paths and anchors
  are not proof that external evidence remains downloadable.
- `git diff --check`: passed.
- Offline backend regressions on the Linux development host: **74 tests run in
  32.677 seconds, OK, one skipped** (the real macOS parent-watcher test). Command:

  ```bash
  PYTHONPATH=backend/src HF_HUB_OFFLINE=1 PYTHONDONTWRITEBYTECODE=1 timeout 90s .venv/bin/python -m unittest discover -s backend/tests -v
  ```

  The first sandboxed attempt stalled and timed out; it is not counted as a pass.
  The permitted rerun outside the sandbox completed successfully. The suite
  covers authentication, locks, shutdown, quantization gating, dataset ownership,
  archive validation, snapshot resolution and training-history persistence among
  other regressions. Many model paths use doubles; no new real-model quality,
  GUI, three-platform package rebuild, or physical-GPU claim follows from this run.
