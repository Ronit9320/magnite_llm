# Local open-source release checklist

Prepared 2026-09-29 from AGENTS.md, the local BUILD_PLAN.md, README, the dated audit and its follow-up, and targeted source review. This is a release plan, not a new full code audit or a fresh test certification.

## Documentation/repository pass — 2026-09-29

Completed without application-source, build-configuration, or dependency changes:
MIT license (owner selected), public guides, synthetic examples, contributor and
security-policy draft, issue/PR templates, CI workflow configuration, dark/light
preview screenshots, dependency/asset inventory, limited local-history scan,
and current baseline verification. See [verification](docs/release/VERIFICATION.md)
and [publication review](docs/release/PUBLICATION_REVIEW.md).

**Owner-deferred:** replace `[PRIVATE_SECURITY_CONTACT_PENDING]` in SECURITY.md
before publication. The user will supply the contact later. CI needs its first
hosted run; icon provenance, clean-machine installation, final support policy,
recorded demo, and remaining source changes are not claimed complete.

## Release scope

Ship the existing native desktop workflow: acquire/manage models, train LoRA/QLoRA adapters locally, manage and activate adapters, serve local inference, and ingest/retrieve documents for RAG chat. Use the Studio 03-based Studio Motion interface; see the [UI polish verification](docs/release/UI_POLISH.md).

Hosted inference, cloud training, provider billing, cloud credentials, and cloud job infrastructure are outside this release. Build-plan phases 7 and 8 are excluded; only the local portions of phases 9 and 10 apply.

“Serving” here means the existing loopback FastAPI service and desktop inference. The custom SSE protocol is not a drop-in OpenAI API. Network serving and `/v1` compatibility would be separate product work, not prerequisites for releasing the app as it is.

Proposed release shape: a source-first preview on a verified platform, with explicit hardware/engine limits. Linux is the first candidate based on the existing evidence. Binary packages and additional operating systems have separate gates below; do not claim support before verifying it.

Testing-package update — 2026-10-09: the Linux v0.1 AppImage has been built and locally verified. Windows `.exe` and macOS `.dmg` packages remain to be built. These are testing artifacts for the unfinished app; public release gates remain open.

## Existing baseline — preserve, do not rebuild

- Model registry, Hugging Face downloads, Ollama integration, and local inference engines exist.
- LoRA/QLoRA training has a serialized queue, cancellation, durable history, and adapter registration.
- Adapter import/export and activation exist. Activation uses a PEFT/torch restart; clearing an adapter unloads inference.
- RAG uses sentence-transformers with SQLite/NumPy retrieval, not ChromaDB/FAISS. Extraction, configuration, retrieval, and chat-context budgeting exist.
- The [audit follow-up](docs/development/AUDIT_2026-09-27.md#follow-up-audit--2026-09-28--dev) records repairs for F1–F16, 24 passing offline tests, desktop feature checks, and a combined real-model workflow. These are historical results to rerun for the release candidate.
- Live vLLM/Ollama, larger-model/OOM behavior, macOS/Windows, and forced-process-death coverage remain unverified in that record.

Unchecked items below are work or evidence still required, not assertions that every corresponding feature is broken.

## P0 — required before the public source release

### 1. Freeze the local product surface

- [x] Remove the disabled Hosted API choice, endpoint/key controls, and hosted-only members from Settings. Preserve cleanup of legacy stored keys. Verified saved indices remain Auto=0, vLLM=1, llama.cpp=2; see UI_POLISH.md.
- [x] Remove cloud/hosted promises from the public roadmap and release copy. Label historical audit/design material as historical where necessary; retain its evidence.
- [x] Publish a feature/compatibility table: GGUF → llama.cpp; transformer weights → torch or separately installed vLLM; Ollama records → Ollama; trained PEFT adapters → torch. State that GGUF inference weights are not the ordinary PEFT training input.
- [x] Document the existing serving endpoints, request examples, streaming terminal/error events, non-streaming responses, loopback binding, and lifecycle. Avoid advertising OpenAI compatibility beyond what the API actually implements.

Completion: every visible release feature is local, implemented, and described accurately.

### 2. Make the repository ready to publish

- [x] Add the owner-selected MIT project LICENSE; third-party material retains separate terms.
- [ ] Inventory third-party code, fonts, icons, and dependencies; add applicable notices and asset provenance. Font OFL files already exist. Record redistribution requirements before bundling binaries.
- [ ] Review tracked files and git history for credentials, private datasets/documents, model weights, personal paths, and internal-only material. The current review did not scan history for secrets.
- [x] Remove superseded design exports, duplicate fonts, unused stylesheets, and stale preview copies. The finalized theme and brand assets live in `app/src/theme.h` and `app/resources/`; contribution rules live in `CONTRIBUTING.md`.
- [x] Add CONTRIBUTING.md with setup, architecture, theme rules, verification commands, and PR expectations.
- [x] Add a draft SECURITY.md and bug/feature/PR templates requesting versions and sanitized logs.
- [ ] Replace the security-contact placeholder (owner will supply it later), confirm a monitored private channel, and finalize supported versions before publication.

Completion: licensing, provenance, contribution, and reporting information are available from a fresh clone, and publication review is recorded.

### 3. Make installation reproducible

- [x] Verify an exported clean source tree and fresh base-only Python 3.12.12 environment on the development host: install, build, 24 regressions, theme, and desktop/IPC tests passed; package versions recorded in docs/release/base-environment.txt.
- [ ] Verify a clean clone and fresh Python environment on the first supported OS; document tested distro, compiler, Qt, Python, and hardware versions plus system packages.
- [ ] Provide tested dependency constraints or a reproducible environment per supported configuration. Current pyproject.toml uses broad lower bounds; a working developer venv is insufficient release evidence.
- [ ] Verify/document CPU llama.cpp installation, including native build prerequisites, and the supported GPU setup. Document vLLM and Ollama installation separately because they are not supplied by the current ML extra.
- [ ] Verify whether the single `[ml]` extra is suitable for each supported platform. Separate training/RAG/quantization extras if necessary, or publish explicit limits; do not promise QLoRA everywhere.
- [x] Document the required `<repo>/build/magnite-app` and `<repo>/.venv` layout for source installs, with actionable diagnostics for missing Python, packages, or backend startup failure.
- [x] Document model/embedding downloads, cache locations, disk needs, HF authentication/gated models, and what works after weights are cached. Explain that MAGNITE_HOME does not relocate the HF cache.

Completion: a new contributor can build and complete a small local workflow using only committed instructions.

### 4. Verify local boundaries and failure recovery

- [ ] Review local IPC access: the backend binds to loopback but currently has no application authentication layer. Verify browser-origin/Host behavior and access to mutating/local-path endpoints; implement protections appropriate to the desktop boundary and document remaining local-user trust assumptions.
- [ ] Verify backend crash, app exit, forced termination, restart, and subprocess cleanup while downloading, loading, generating, training, and ingesting. Check that incomplete work is not presented as successful or left registered as usable.
- [ ] Exercise missing dependencies, invalid/incompatible models and adapters, missing/corrupt inputs, disk-full/permission errors, offline downloads, and OOM. Errors must be visible and leave controls usable.
- [ ] Verify inference and training sharing a GPU. Training jobs are serialized with each other, but this alone does not establish safe resource sharing with inference; document or enforce the supported behavior.
- [ ] Verify deletion and import/export on disposable stores, including active resources and shared HF blobs. Preserve the existing archive-validation and cache-aware deletion regressions.

Completion: no known data-loss, unauthorized local-file operation, orphan-process, or silent-failure blocker remains in the supported workflows; limitations are explicit.

### 5. Establish release verification

- [x] Configure CI for offline backend regressions, CMake build, theme audit, and the bare desktop selftest with loopback access.
- [ ] Run the workflow on GitHub and fix any hosted-environment issues; YAML validation and local passes do not establish hosted CI success.
- [x] Verify the current baseline: 24 regressions, build, theme, bare desktop test, all five feature tests, and the combined real workflow pass; see docs/release/VERIFICATION.md.
- [x] Run all five feature selftests on the UI candidate with disposable MAGNITE_HOME and XDG_CONFIG_HOME: pull, inference, training, adapters, and RAG. Passed 2026-09-30; see docs/release/UI_POLISH.md. Rerun after subsequent functional changes.
- [ ] Run `backend/tests/live_workflow.py` for the combined train → adapter → RAG chat → export/import workflow.
- [ ] Test representative usable models and a mixed document corpus in addition to tiny random models. Record memory, latency, cancellation, retrieval relevance, and context-limit behavior; token delivery alone does not establish answer quality.
- [ ] Verify live vLLM/Ollama if they are advertised as supported; otherwise label those integrations experimental and keep the supported path clear.
- [x] Smoke-test light/dark modes, keyboard navigation, reduced motion, display scaling, and long/error content across all six panels. Offscreen checks passed at 1×/2×; native desktop and assistive-technology review remain separate.
- [ ] Store a release verification report with commit, environment, commands, results, and known limitations. Temporary `/tmp` logs from previous work are not release evidence available to users.

Completion: reproducible checks pass for the exact candidate commit and its advertised support matrix.

### 6. Finish release content

- [x] Rewrite the README around the local product, accurate verification-status matrix, real clone URL, source-build quickstart, data locations, and limitations. Clean-install verification remains open separately. Include RAG in the architecture diagram.
- [x] Use committed user/developer documentation for contribution instructions; local planning files are not required. Current theme rules are in `CONTRIBUTING.md`.
- [x] Add concise walkthroughs for model download/load/chat, dataset preparation/training, adapter activation/export/import, and document ingestion/query/RAG chat. Include small redistributable sample inputs with provenance.
- [x] Add troubleshooting for backend startup, engine/model mismatch, CUDA/quantization installation, memory limits, embedding downloads, empty retrieval, and cooperative cancellation.
- [x] Document storage/deletion/backup behavior, settings restart requirements, retained histories, and current chat persistence limits. Describe actual outbound connections without implying downloads are offline.
- [x] Capture current empty-state screenshots in both themes with disposable data/settings.
- [ ] Refresh screenshots after source cleanup and record a populated end-to-end demo using docs/release/DEMO.md; avoid credentials, private paths, datasets, and unsupported claims.
- [x] Confirm existing CMake, Python, and FastAPI versions agree at 0.1.0; prepare CHANGELOG and draft release notes with features, installation, limitations, and verification links.
- [ ] Select the final release version/tag/date and finalize release notes after the source changes; no tag or release was created.

Completion: a user can understand, install, use, and troubleshoot the released product without local planning files or maintainer assistance.

## P1 — usability improvements after blockers, before a broad stable launch

- [ ] Add independently managed download/ingestion jobs with visible stages, progress, cancel, and retry. Current cooperative cleanup waits for native calls to return; do not describe cancel as immediate.
- [ ] Expose RAG similarity threshold and embedding-model state coherently; add a deliberate reindex flow for model changes. Current configuration guards should remain until safe reindexing exists.
- [ ] Show retrieved passages and document sources alongside RAG answers so users can inspect grounding.
- [x] Correct the 8-bit training UI label to “8-bit (bitsandbytes)”.
- [ ] Correct RAG schema descriptions that call word-based chunk sizes “tokens”; review related counters/labels for the same mismatch.
- [ ] Measure corpus limits and publish them; improve batching/retrieval scale only where measurements justify it.
- [ ] Add dataset preview/validation feedback and a training summary users can inspect before starting expensive runs.
- [ ] Improve backend recovery and user-accessible diagnostic logs with sensitive-content handling.
- [ ] Add chat save/restore/export, window/layout persistence, and last-used selections as appropriate; avoid silently auto-loading large models on launch.
- [ ] Expand regression coverage around supported model templates, long conversations, repeated load/unload, restart recovery, and resource contention based on observed failures.

These are prioritized improvements, not a reason to restart implemented systems or indefinitely delay an explicitly limited preview.

## Binary distribution — required only for each advertised package

### Linux AppImage — test build completed

- [x] Make backend/Python/resource discovery work outside the repository layout and add CMake installation/AppImage packaging rules. The Linux package uses an adjacent isolated Python runtime; see [packaging instructions](packaging/README.md).
- [x] Build `dist/Magnite-0.1.0-test.1-x86_64.AppImage` with Qt, Python and CPU inference/LoRA/RAG dependencies. Built 2026-10-08 on `release/v0.1-testing`, commit `6c83de5`; approximately 418 MiB. Model weights download separately.
- [x] Verify relocated startup, theme/UI checks, GGUF inference and the combined CPU LoRA/adapter/RAG workflow. Record checksums, resolved dependencies and build provenance locally. See [Linux test-build verification](docs/release/V0_1_LINUX_TESTING.md).
- [ ] Test on a clean second Linux machine, including native desktop integration, Qt plugins, backend process cleanup and removal behavior. Current build requires x86-64 and glibc 2.43+; build on an older distribution and verify separately before advertising broader compatibility.

### Windows .exe — pending

- [ ] Prepare a Windows x64 build environment and compile the Qt client; bundle Qt DLLs/plugins, standalone Windows Python, the backend and tested CPU ML dependencies.
- [ ] Produce a v0.1 testing installer `.exe`, with application shortcuts and uninstall support. Record exact source/dependency versions and a SHA-256 checksum.
- [ ] Verify installation and launch on a clean Windows machine without a developer checkout or system Python. Test inference, CPU LoRA, adapters, RAG, backend process cleanup, upgrade and uninstall while preserving user data.
- [ ] Complete signing and installer verification for public distribution; identify unsigned testing builds explicitly.

### macOS .dmg — testing build produced; not release-ready

- [x] Select and document the initial architecture (Apple Silicon and/or Intel) and minimum macOS version; prepare a native macOS build environment. *(Apple silicon arm64, macOS 14.0+: `docs/release/macos/adr/0001-target-architecture.md`, `0002-minimum-macos.md`, `environment-macos.txt`.)*
- [x] Build a self-contained `.app` containing Qt frameworks/plugins, Python, the backend and platform-compatible ML dependencies; package it as a v0.1 testing `.dmg`. Record exact source/dependency versions and a SHA-256 checksum. *(`dist/Magnite-0.1.0-test.1-arm64.dmg`, SHA-256 `48050836…`; see `docs/release/macos/V0_1_MACOS_TESTING.md` and LOG.md Phases 5, 7 and the ISSUE-011 entry. Built from a modified tree, so not claimed reproducible from a commit.)*
- [ ] Verify installation and launch on a clean Mac without a developer checkout or system Python. Test supported inference/training/RAG paths, backend process cleanup, upgrade and removal while preserving user data. Record Metal/MPS and quantization limits separately.
  *Not ticked: the app was verified from the dmg (11/11) and in a second standard account (6/6), with Metal/MPS and the QLoRA limit recorded in `ML_MATRIX.md`, but never on a Mac free of Homebrew/system Python, the Gatekeeper dialog wording was not captured, and the cross-account API check was skipped. See LOG.md Phases 8–10.*
- [ ] Complete Developer ID signing, notarization and Gatekeeper verification before public distribution. *DEFERRED by the maintainer (2026-10-30); users approve the app via `docs/MACOS_FIRST_LAUNCH.md`. Would need an Apple Developer Program enrollment, a Developer ID Application certificate and a `notarytool` profile. The test build is ad-hoc signed.*

### Shared publication gates

- [ ] Publish package checksums, build provenance, third-party notices, and a documented upgrade/rollback path that preserves user data.
- [ ] Publish only verified platform packages as release assets, with installation instructions and explicit OS/hardware/engine limits. Link download-page buttons to those assets if a website is added.

Gate each platform separately. Completing a local test package does not establish public-release readiness, and three-platform installers are not a prerequisite for a Linux source preview.

## Explicitly deferred

- Hosted providers, cloud training, remote credentials, billing, and cloud dispatch.
- Public/LAN inference serving and a full OpenAI-compatible API.
- Native vLLM multi-LoRA and llama.cpp adapter conversion/hot-switching.
- A vector-database replacement without measured need.
- Unrelated new product features. The subsequently requested UI refinement is recorded in docs/release/UI_POLISH.md.

## Execution order and release gate

1. Freeze scope and remove hosted surfaces/promises.
2. Resolve license/publication decisions; make fresh-clone setup and documentation complete.
3. Establish CI and close supported-workflow hardening gaps.
4. Run candidate verification; finish screenshots, examples, release notes, and known limitations.
5. Publish the source preview only after all P0 items are complete or explicitly narrowed by documented release scope. Add binaries only after their separate gates pass.

Product decisions still needed during execution: private reporting contact, final supported OS/hardware matrix, and source-only versus packaged first release. MIT is selected and documentation uses the existing GitHub origin; repository visibility is unchanged. These do not block preparing the rest of the release work.
