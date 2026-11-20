# Preview limitations

Reviewed 2026-11-20 for magnite-spacev0.2 (v0.2-preview).
See [native package evidence](development/NATIVE_PACKAGES.md) for exact builds
and [the release checklist](../RELEASE_CHECKLIST.md) for remaining acceptance work.

## Distribution and platform coverage

- v0.2-preview publishes a Linux x86-64 CPU AppImage, a macOS Apple-silicon DMG
  and a Windows x86-64 per-user CPU installer from one source commit, each with
  signed in-app preview updates.
- Blueprints, Tool mode and the bundled Node.js/Pi worker are switched off in this
  release. They remain in development on `dev`.
- The macOS build is ad-hoc signed and not notarized; the Windows installer is
  not Authenticode-signed. Both systems warn on first launch
  ([macOS](MACOS_FIRST_LAUNCH.md), [Windows](WINDOWS_FIRST_LAUNCH.md)). Windows
  Smart App Control blocks the installer entirely.
- In-app updates: Linux and macOS roll back a version that does not start;
  Windows reports it but does not roll back. macOS can ask for App Management
  permission, and an app opened from the disk image or a read-only location
  can't update itself. The macOS and Windows install steps are covered by hosted
  checks and script tests, not yet by an update between two published releases
  on physical machines.
- v0.1-preview cannot update to v0.2-preview in place because `DATA_SCHEMA`
  changed; install the new package manually. There is no migration framework.
- Linux CI builds on Ubuntu 22.04 and gates ELF glibc requirements at 2.35.
  Broader native desktop/distro checks remain open.
- Hosted checks do not establish clean consumer-machine installation, every OS
  version, or physical GPU support. Linux and Windows packages are CPU builds;
  the macOS package uses Metal/MPS, checked on an M1 with small models.
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
- Ordinary chat adapter activation restarts the Torch engine. Blueprint Model
  nodes can select compatible PEFT adapters within a run. Clearing the chat
  adapter unloads inference. [Blueprint capabilities](BLUEPRINTS.md#supported-runtimes-and-checkpoints)
- Cancellation is cooperative. Download progress and independent job
  management need further work; model-pull history is not durable and has no cancel API.
  RAG ingest and reindex report progress (`GET /rag/activity`) and stop within one
  embedding batch; the Knowledge panel shows that progress with a Cancel button.
- RAG indexing-time hints are estimates: until a real ingest or reindex has run, the
  Knowledge panel uses speeds measured on an Apple M1 with 8 GB, which can be far off on
  other hardware (especially CUDA). The measured speed is kept in memory only and is
  lost when the backend restarts.
- RAG: scanned/image-only PDFs have no OCR, and `pypdf` extraction can mangle tables and
  multi-column layouts. Retrieval is exact and in memory (float16 index capped at 512 MB,
  about 250,000 chunks at 1,024 dimensions; larger libraries are streamed from disk and
  slow down); there is no approximate index or reranker, and collections beyond roughly
  300,000 chunks need a different design. Retrieval is hybrid (SQLite FTS5 keyword plus
  vector, fused with Reciprocal Rank Fusion) and falls back to vector only when the SQLite
  build lacks FTS5. Keyword matching is weak for Chinese, Japanese and Korean text, which
  the tokenizer indexes as whole runs; the vector search still covers it. `min_score` is a
  cosine-similarity floor, so an exact-term match with a low similarity is dropped when a
  threshold is set (the default, 0, keeps everything).
- RAG embedding models: the long-context presets (BGE-M3, Qwen3-Embedding) need about
  3 GB of RAM and index only about 2 chunks per second on an 8 GB Apple M1, so 8 GB
  machines default to the 256-token MiniLM with ~150-word chunks, which finds long-document
  details less precisely. Embedding stops with an error if free memory is too low. The
  quality comparison used 47 questions, partly synthetic, and should be rerun on your own
  documents (`backend/evals/`).
- Large-corpus retrieval beyond 50,000 chunks, large-model OOM recovery, active-work forced termination,
  and concurrent training/inference need broader verification. Existing lifecycle
  checks do not establish every engine/operation combination.
- Interrupted training requires a new job. Restore chat history and other
  local records with the same application revision and path layout first.
  `DATA_SCHEMA` gates update compatibility.
  [Storage and backup](STORAGE_AND_PRIVACY.md)
- Tiny-model test passes establish workflow delivery, not useful answer quality
  or fine-tuning effectiveness. Recorded M1 Metal/MPS results do not establish
  all Apple/GPU/model configurations.
