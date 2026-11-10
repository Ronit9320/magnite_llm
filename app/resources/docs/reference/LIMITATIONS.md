# Preview limitations

Reviewed 2026-11-02 against the integrated source and recorded verification.
See [native package evidence](development/NATIVE_PACKAGES.md) for exact builds
and [the release checklist](../RELEASE_CHECKLIST.md) for remaining acceptance work.

## Distribution and platform coverage

- The public `v0.1-preview` release provides a Linux x86-64 CPU AppImage with
  signed preview updates. Clean-source Linux AppImage, Mac ARM DMG and Windows
  x86-64 installer candidates passed the native integration workflow recorded
  on 2026-10-30. Mac/Windows candidates are testing artifacts, not public releases.
- Linux CI builds on Ubuntu 22.04 and gates ELF glibc requirements at 2.35.
  The old local test AppImage's glibc 2.43 requirement is historical, not the
  current CI package baseline. Broader native desktop/distro checks remain open.
- Mac candidates target Apple silicon/macOS 14+, are ad-hoc signed and not
  notarized. Windows candidates are unsigned per-user CPU installers. Hosted
  checks do not establish clean consumer-machine installation, every OS version,
  or physical GPU support. Platform updaters/publication remain separate work.
- Source installation depends on the checkout layout. Packaging uses isolated
  Python runtimes and platform dependency locks; editable source installs retain
  broad dependency bounds unless constraints are explicitly supplied.

## Local API and privacy

- The local FastAPI API exists; see [API examples](LOCAL_API.md). It uses custom
  routes/SSE frames and is not a drop-in OpenAI API. Public/LAN serving is outside scope.
- Every desktop launch supplies a new `MAGNITE_API_TOKEN`; all routes except
  `/health` require its Bearer credential. The network middleware rejects
  non-loopback `Host` values. A hand-started backend without a token has no
  credential check. These protections do not sandbox code running as your user.
- One backend per data directory is enforced with POSIX/Windows file locks.
  Lifecycle/authentication regressions and native package checks are recorded
  separately from broader security certification. No encryption-at-rest or
  application telemetry feature is implemented.

## Models, training, and retrieval

- vLLM and Ollama integrations have regression coverage but need live release checks.
- Training is LoRA/QLoRA adapter training, not cloud/full-model training. QLoRA
  needs supported hardware and bitsandbytes; it is explicitly unsupported on macOS.
- Adapter activation restarts torch; native multi-LoRA and GGUF adapter conversion
  are not implemented. Clearing an adapter unloads inference.
- Cancellation is cooperative. Download/ingestion progress and independent job
  management need further work; model-pull history is not durable and has no cancel API.
- RAG chunk sizes are words; some schema descriptions still say tokens. PDF OCR
  and automatic embedding-model reindexing are absent. Clear/re-ingest is required
  to change embedding models while documents exist.
- Large-corpus retrieval, large-model OOM recovery, active-work forced termination,
  and concurrent training/inference need broader verification. Existing lifecycle
  checks do not establish every engine/operation combination.
- Chat history is not durable and interrupted training does not automatically
  resume. Backup/restore across changed paths or future schema versions needs
  manual care. `DATA_SCHEMA` is an updater compatibility gate, not a migration system.
- Tiny-model test passes establish workflow delivery, not useful answer quality
  or fine-tuning effectiveness. Recorded M1 Metal/MPS results do not establish
  all Apple/GPU/model configurations.
