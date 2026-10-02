# Magnite LLM architecture

This document describes the implementation in this checkout, reviewed on 2026-10-02. It is a code-oriented guide to responsibilities, process boundaries, data flows, persistence, and extension points. Planned features and historical design phases are not treated as implemented functionality.

Magnite is a native C++20/Qt 6 desktop application backed by a Python FastAPI service. The desktop owns presentation and interaction; Python owns model management, inference, adapter training, and document retrieval. The supported product scope is local inference, LoRA/QLoRA training, adapters, and RAG. Hosted inference and cloud training are outside that scope.

## Contents

- [System boundaries](#system-boundaries)
- [Repository map](#repository-map)
- [Startup and shutdown](#startup-and-shutdown)
- [Desktop architecture](#desktop-architecture)
- [Backend composition and API](#backend-composition-and-api)
- [Model management](#model-management)
- [Inference and streaming](#inference-and-streaming)
- [Datasets and training](#datasets-and-training)
- [Adapter lifecycle](#adapter-lifecycle)
- [Retrieval and document processing](#retrieval-and-document-processing)
- [Persistence and configuration](#persistence-and-configuration)
- [Concurrency and failure behavior](#concurrency-and-failure-behavior)
- [Trust and network boundaries](#trust-and-network-boundaries)
- [Build and verification](#build-and-verification)
- [Extension points and constraints](#extension-points-and-constraints)

## System boundaries

```text
Desktop process: magnite-app
  QApplication -> MainWindow -> six workspace panels
                     |                    |
               BackendManager       BackendClient
                  QProcess          JSON HTTP / SSE
                     |                    |
                     v                    v
Python process: python -m backend.main
  Uvicorn / FastAPI on 127.0.0.1:<random port>
    routes -> model registry / training manager / inference supervisor / RAG store
                  |                 |                    |
            SQLite + files    training thread      active engine
                                                        |
                      +-----------------+---------------+---------------+
                      |                 |               |               |
                 llama.cpp         torch + PEFT      vLLM child     Ollama service
                 in process        in process       HTTP / SSE     HTTP / NDJSON
```

The desktop does not load ML libraries or execute model computation. Its only backend integration is the local HTTP API and the supervised Python process. Most ML execution happens inside Python; vLLM runs as an additional managed subprocess, while Ollama is an independently installed and running service.

There is one application-wide inference supervisor and at most one active inference engine per backend process. Training has its own serialized worker queue. This separation does **not** establish a shared GPU resource scheduler: training and inference can compete for the same device.

The implementation assumes a single desktop-oriented backend process. Module-level service instances, in-memory jobs, and process-local locks are not designed as a distributed or multi-worker service.

## Repository map

| Location | Responsibility |
| --- | --- |
| [`app/CMakeLists.txt`](app/CMakeLists.txt) | C++20 executable, explicit source list, Qt dependencies, automatic Qt resource/meta-object processing |
| [`app/src/main.cpp`](app/src/main.cpp) | QApplication identity, theme setup, command-line verification modes, window creation |
| [`app/src/mainwindow.cpp`](app/src/mainwindow.cpp) | Navigation, service wiring, panel settings, backend status, desktop selftests and screenshots |
| [`app/src/backend_manager.cpp`](app/src/backend_manager.cpp) | Python process launch, port discovery, readiness, termination |
| [`app/src/backend_client.cpp`](app/src/backend_client.cpp) | Asynchronous JSON requests, SSE parsing, request aborts, atomic downloads |
| [`app/src/panels/`](app/src/panels/) | Models, Training, Knowledge/RAG, Adapters, Chat, Settings and their card/bubble widgets |
| [`app/src/widgets/`](app/src/widgets/) | Shared presentation primitives, layouts, motion, branding, confirmations |
| [`app/src/theme.h`](app/src/theme.h), [`app/src/theme.cpp`](app/src/theme.cpp) | Theme tokens, resource loading, icon tinting, theme audit |
| [`app/resources.qrc`](app/resources.qrc), [`app/resources/`](app/resources/) | Embedded stylesheet, fonts, icons and other assets |
| [`backend/src/backend/main.py`](backend/src/backend/main.py), [`server.py`](backend/src/backend/server.py) | CLI, Uvicorn startup, FastAPI composition and lifecycle hooks |
| [`backend/src/backend/routes/`](backend/src/backend/routes/) | HTTP contract and translation between transport and application services |
| [`backend/src/backend/models/`](backend/src/backend/models/) | Hub/Ollama integration, download jobs, model registry and schemas |
| [`backend/src/backend/inference/`](backend/src/backend/inference/) | Engine selection, single-engine ownership, inference request schemas and engine implementations |
| [`backend/src/backend/training/`](backend/src/backend/training/) | Dataset normalization, training queue, Trainer integration and job schemas |
| [`backend/src/backend/adapters/`](backend/src/backend/adapters/) | Adapter records and SQLite persistence; archive operations live in the adapter router |
| [`backend/src/backend/rag/`](backend/src/backend/rag/) | Text extraction, chunking, embeddings, vector retrieval and prompt context |
| [`backend/src/backend/request_work.py`](backend/src/backend/request_work.py) | Disconnect-aware ownership of long-running worker-thread operations |
| [`backend/tests/`](backend/tests/) | Offline regressions and live workflow verification |
| [`app/src/testing/`](app/src/testing/) | UI interaction and layout checks |
| [`docs/`](docs/), [`design/`](design/) | Public usage/release documentation and committed design rules |
| `shared/` | Placeholder; no generated cross-language API schema is implemented here |

The HTTP contract is expressed in Python routes/Pydantic models and manually consumed by C++ JSON code. There is no generated typed C++ client. Changes to payload names therefore require coordinated edits on both sides.

## Startup and shutdown

### Desktop startup

1. `main.cpp` creates `QApplication`, setting the QSettings organization to `Magnite` and application name to `Magnite LLM`.
2. Theme resources and fonts are initialized. Special verification modes can run before a normal window is created.
3. `MainWindow` constructs the workspace, shared `BackendClient`, and `BackendManager`, restores panel preferences, and starts the backend.
4. `BackendManager` derives the repository root from the executable directory. The expected source layout is `<repo>/build/magnite-app`, `<repo>/backend/src`, and `<repo>/.venv`.
5. It prefers `.venv/bin/python` (`.venv/Scripts/python.exe` on Windows), with `python3` as fallback, and launches `-m backend.main --port 0`. It prepends `backend/src` to `PYTHONPATH`.
6. It passes the configured engine preference as `MAGNITE_ENGINE_OVERRIDE`. A saved data-directory preference supplies `MAGNITE_HOME` only if that environment variable is not already set.
7. Python selects a free loopback port and prints `Magnite backend starting on http://127.0.0.1:<port>` to stderr. The desktop buffers complete stderr lines, forwards logs, and extracts the port from this announcement.
8. The desktop retries `GET /health` until successful while the process remains running. The `started` signal then gives the shared client its base URL and connects the panels to it.

The announcement is a discovery protocol, not a readiness guarantee. `/health` confirms the service can answer requests; it does not prove that weights are loaded or that a particular ML dependency/device works. Port selection probes and releases a socket before Uvicorn binds it, rather than passing an already-bound socket to the server.

### Backend composition

[`server.py`](backend/src/backend/server.py) creates one FastAPI app and registers all routers. Its startup hook initializes:

- `ModelStore` and `ModelRegistry` for model metadata and pulls.
- `DatasetStore`, an `AdapterStore`, and `TrainingJobManager` for training.
- A second `AdapterStore` instance over the same `adapters.db` for adapter routes and inference.
- `RagStore` and its module-level accessor.
- The singleton `InferenceSupervisor`, wired to the adapter store.

The two adapter-store objects share durable records through SQLite, not a shared Python object. Their Python locks are per instance.

### Shutdown and unexpected exit

The FastAPI shutdown hook asks the training manager to cancel/shut down, then unloads inference using worker threads. Training shutdown sets cancellation events and waits up to two seconds for its daemon worker.

The desktop requests Python termination and waits up to 15 seconds, then kills it if necessary and waits another three seconds. On Linux, Python makes a best-effort `prctl` parent-death-signal request so a dead desktop also terminates its backend. vLLM has its own terminate/kill cleanup and an `atexit` cleanup registration.

An unexpected backend exit causes the desktop to abort outstanding requests, clear the client base URL, and display an unavailable/error state. There is no automatic crash-restart loop. Forced termination does not guarantee that all native ML work or file writes complete cleanly.

## Desktop architecture

`MainWindow` is the composition root for six panels in a `QStackedWidget`. Navigation uses the tool rail and Ctrl+1–6 shortcuts. Panels derive from `BasePanel`, which supplies the common header, content layout, status styling, and optional JSON settings serialization.

| Panel | Main responsibilities | Main API families |
| --- | --- | --- |
| `ModelsPanel` | Registry cards, Hub search, pulls, deletion, workspace overview, sending a selected model to Chat | `/models`, overview reads from other services |
| `TrainingPanel` | Base-model and dataset selection, dataset import, training configuration, progress/loss display, cancellation | `/models`, `/datasets`, `/training` |
| `RagPanel` | Document ingestion/deletion, retrieval configuration, query experiments | `/rag` |
| `AdaptersPanel` | Adapter listing, activation/clearing, deletion, ZIP import/export | `/adapters`, `/inference/adapter`, `/inference/status` |
| `ChatPanel` | Model loading, sampling settings, in-memory conversation, streaming response rendering, stop/new chat, optional retrieval | `/models`, `/inference` |
| `SettingsPanel` | Engine preference, data-directory preference, ambient motion and reduced motion | QSettings; MainWindow forwards runtime engine changes |

Panels use asynchronous callbacks rather than blocking on HTTP. Model pulls and training jobs are polled with Qt timers; inference tokens arrive over SSE. MainWindow also wires domain changes through Qt signals: model changes refresh Chat/Training choices, training completion refreshes Adapters, and adapter activation refreshes Chat's engine status.

Chat keeps message history in `ChatPanel::m_history` and sends that history with each request. The backend has no persistent conversation/session service. The panel tracks request identities so callbacks from aborted or replaced requests do not mutate a newer conversation. Stop aborts the streaming network reply; backend cleanup then cooperatively stops generation.

### Presentation and theme

Colors and other design tokens are centralized in `theme.h`. `Theme::qss()` substitutes `@token` placeholders in `app/resources/theme/console.qss`, embedded through the `:/theme/theme.qss` resource alias. `Theme::coloredIcon()` tints SVGs using `currentColor`. Fonts and icons are bundled through Qt resources.

Shared widgets implement surfaces, cards, flow layout, click feedback, branding and ambient animation. Light/dark selection and reduced-motion preferences are persisted. The active contribution rules are in [Studio 03](design/studio-3/README.md), with subsequent layout/motion refinements in [Studio Motion](design/intuitive-motion/README.md). Historical phase names in comments do not supersede these rules.

## Backend composition and API

Routers handle HTTP validation, status codes, service lookup, and response construction. Pydantic models define request/response shapes; service and store classes contain most workflow and persistence logic. Some routers also contain substantial orchestration, notably adapter archive handling and inference stream/context preparation.

FastAPI synchronous handlers run through its thread-pool mechanism. Async inference and document-ingestion handlers explicitly move blocking work into workers where needed. Most heavy ML imports occur inside the engines/trainer/embedder, allowing the base service to start without the optional ML extras.

### Endpoint inventory

Paths below are relative to the dynamically discovered loopback base URL. `{id}` placeholders stand for the corresponding model, job, dataset, adapter, or document identifier.

| Area | Endpoints | Behavior |
| --- | --- | --- |
| Health | `GET /health` | Service status/version |
| Models | `GET /models`, `GET /models/search`, `GET /models/{id}`, `DELETE /models/{id}` | List/filter, Hub search, details, managed deletion |
| Pulls | `POST /models/pull`, `GET /models/pull/{id}` | Start with HTTP 202 and poll an in-memory job |
| Inference lifecycle | `GET /inference/status`, `POST /inference/load`, `POST /inference/unload` | Inspect or replace the active engine/model |
| Inference preferences | `POST /inference/settings` | Set runtime engine override for subsequent selection |
| Adapter selection | `POST /inference/adapter` | Activate an adapter; empty `adapter_id` clears/unloads it |
| Generation | `POST /inference/chat`, `POST /inference/completion` | SSE by default; collected JSON when `stream=false` |
| Datasets | `POST /datasets/ingest`, `GET /datasets`, `GET /datasets/{id}`, `DELETE /datasets/{id}` | Normalize/import, list, inspect, remove |
| Training | `POST /training/start`, `GET /training/jobs`, `GET /training/status/{id}`, `POST /training/cancel/{id}` | Submit with HTTP 202, inspect, cooperatively cancel |
| Adapter registry | `GET /adapters`, `GET /adapters/{id}`, `DELETE /adapters/{id}` | List/filter, inspect, remove weights/metadata |
| Adapter archives | `POST /adapters/export/{id}`, `GET /adapters/export/{filename}`, `POST /adapters/import` | Create ZIP, download ZIP, import a local ZIP path |
| RAG configuration | `GET /rag/status`, `GET /rag/config`, `POST /rag/config` | Capability/counts and persisted configuration |
| RAG documents | `GET /rag/documents`, `POST /rag/documents`, `DELETE /rag/documents/{id}`, `POST /rag/clear` | Manage the indexed collection |
| Retrieval | `POST /rag/query` | Return ranked chunks and similarity scores |

Dataset and document ingestion accept local filesystem paths or inline content. Adapter import accepts a local archive path. These are local-service operations, not multipart upload endpoints. FastAPI also provides its standard generated OpenAPI/docs endpoints.

See [LOCAL_API.md](docs/LOCAL_API.md) for request examples. Source routes remain authoritative for individual validation rules and error codes: error mapping is not uniform across all endpoints.

### Desktop transport

`BackendClient` wraps one `QNetworkAccessManager` and exposes `get`, `post`, `remove`, `streamPost`, `download`, `abortStream`, and `abortAll`. Ordinary JSON and streaming requests use a 600-second transfer timeout; exports use a 60-second transfer timeout. Empty 204 responses yield an empty JSON document. Transport failures and non-JSON bodies are translated into error/detail objects for panel callbacks.

Downloads stream into `QSaveFile` and commit only after success, preserving an existing destination file on failure. The client tracks one current streaming reply, matching the single Chat workspace.

## Model management

[`ModelRegistry`](backend/src/backend/models/registry.py) combines persistent metadata with integrations for Hugging Face and Ollama. [`ModelStore`](backend/src/backend/models/store.py) stores model records in SQLite. A record includes source, original reference, local path/service handle, architecture, size, license, status, and extra metadata.

Registry IDs use `source:reference`, with `/` replaced by `--`; for example, `huggingface:owner--model`. These IDs are different from inference engine names and filesystem paths.

For a Hugging Face pull:

1. `POST /models/pull` creates a short UUID job ID and starts a daemon thread.
2. `snapshot_download()` downloads a repository snapshot through the Hub cache. A tqdm bridge updates job progress state.
3. The registry inspects the snapshot and Hub metadata, then upserts a ready model record.
4. The desktop polls the job until `done` or `error`, then refreshes its model list.

Pull jobs are in memory and disappear on backend restart. There is no pull-cancellation endpoint or durable download queue. Pulls request repository snapshots, not a UI-selected individual weight file.

For Ollama, the registry queries `/api/tags`, streams `/api/pull`, and uses `/api/delete`. Listing models can refresh Ollama records, so a registry read may contact the configured service. Ollama owns its weight storage; Magnite stores model handles. Refresh upserts discovered records rather than implementing a complete authoritative reconciliation of externally removed models.

Model deletion checks the active inference reference and queued/running training references. It refuses a matching in-use model. Hugging Face deletion identifies a managed cache revision and uses cache-aware deletion; shared blobs can remain. Ollama deletion requests actual deletion from its service before removing the registry record.

## Inference and streaming

[`InferenceSupervisor`](backend/src/backend/inference/supervisor.py) owns engine discovery, reference resolution, lifecycle changes, active-adapter state, and generation exclusivity. [`BaseEngine`](backend/src/backend/inference/engines/base.py) defines `load`, `unload`, `chat_stream`, `complete_stream`, token counting and context capacity.

| Engine | Execution model | Inputs and behavior |
| --- | --- | --- |
| `llamacpp` | `llama-cpp-python` in the backend process | Resolves GGUF weights; streams native generation through a worker bridge; default context size is 2048 |
| `torch` | Transformers and optional PEFT weights in the backend process | Loads a tokenizer and causal LM; chooses CUDA, MPS, or CPU; uses a text streamer and generation thread |
| `vllm` | Managed Python child process | Starts vLLM's OpenAI-compatible server on another loopback port; proxies streamed results over HTTP; logs stderr to `vllm.log` |
| `ollama` | Existing external service | Checks the model with `/api/show`, translates chat/completion calls to Ollama, parses NDJSON deltas, requests unload with `keep_alive=0` |

vLLM startup has a 240-second readiness budget. Its OpenAI-compatible upstream protocol does not make Magnite's public API OpenAI-compatible. Ollama capability is advertised as available without proving the service is currently reachable.

### Selection and loading

The supervisor detects Apple Silicon or probes NVIDIA hardware with `nvidia-smi`, and checks engine dependency availability. Selection follows these main rules:

- A registered Ollama model resolves to the Ollama engine; an incompatible explicit engine is rejected.
- A registered Hugging Face ID resolves to its existing local path, or falls back to the repository reference.
- Without an explicit engine or global override, a local directory containing GGUF files selects llama.cpp. Other model directories select vLLM when available with a GPU, otherwise torch.
- Explicit request engines are honored after validation. Global overrides are `0=Auto`, `1=vLLM`, `2=llama.cpp`.
- The general automatic preference favors vLLM on supported non-macOS GPU setups, then llama.cpp. This is a heuristic, not a universal model-format compatibility guarantee.

Loading first unloads the existing engine and clears its adapter, then constructs and loads the replacement. The new engine is published only after successful loading and a cancellation check. A failed replacement does not restore the previous model.

Generation uses the currently loaded model. The optional `model` field on chat/completion schemas does not itself switch models; callers must use `/inference/load` first.

### Generation flow and SSE contract

```text
ChatPanel history + user message + sampling options
  -> POST /inference/chat
  -> reserve supervisor operation lock
  -> validate context budget; optionally retrieve document context
  -> engine async iterator
  -> fetch first delta before committing response headers
  -> SSE frames -> BackendClient parser -> ChatBubble updates
  -> iterator cleanup -> release operation lock
```

Magnite's SSE frames contain JSON on `data:` lines and are separated by a blank line:

```text
data: {"delta": "Hello"}

data: {"delta": " there"}

data: {"done": true}

```

Failures after streaming begins use `data: {"error": "..."}`. Fetching the first item before sending headers lets initial validation/loading/generation failures produce HTTP errors instead. The client accumulates network bytes until complete events are available, so transport chunk boundaries need not match token/event boundaries.

With `stream=false`, the route consumes the same generator and returns a JSON completion object with `choices`, timestamps, model and an ID. Chat choices contain an assistant message; completion choices contain text.

Context validation reserves the requested output budget. Torch uses its tokenizer and formatted chat prompt; llama.cpp supplies token counts but inherits message-overhead estimation. Engines without specialized counting inherit a conservative UTF-8 byte count and a 2048 context fallback. These checks are not exact upstream token accounting for every engine.

## Datasets and training

[`DatasetStore`](backend/src/backend/training/datasets.py) accepts UTF-8 JSONL, CSV and plain text, normalizes usable samples into text or chat-message objects, writes `datasets/<id>.jsonl`, and stores metadata in `datasets.db`. Instruction/input/output examples are converted into user/assistant messages. Invalid structures and empty datasets are rejected. Original source files remain separate from normalized copies.

[`TrainingJobManager`](backend/src/backend/training/jobs.py) validates dataset existence and configuration, creates a job plus cancellation event, persists it, and queues it for a single daemon worker. Only one training job executes at a time. The queue is in-process; there is no external job broker or dedicated training process.

Job states are `queued`, `running`, `done`, `error`, and `cancelled`. The API exposes progress, steps, loss, bounded loss history, messages, timestamps, configuration, and a resulting adapter ID.

[`run_training`](backend/src/backend/training/trainer.py) performs the workflow:

1. Import the ML dependencies and resolve the base-model reference.
2. Load tokenizer and causal LM. Configure bitsandbytes for requested 4-bit/8-bit training.
3. Prepare quantized models for k-bit training and create the PEFT LoRA configuration.
4. Resolve target module names against the actual model; fall back to `all-linear` when explicit names do not match.
5. Convert normalized samples to text using the tokenizer chat template where possible; tokenize with configured truncation/padding.
6. Construct Transformers `TrainingArguments`, a causal-language-model data collator, and a `TrainerCallback` progress bridge. The public `warmup_ratio` is converted into `warmup_steps`.
7. Run Trainer, checking cancellation at setup boundaries and training step boundaries.
8. Save adapter weights and, best-effort, tokenizer files under the adapter directory; register an `AdapterRecord`; mark the job done.

The training method produces adapters, not merged full-model exports. Quantized training still requires a compatible runtime/device. Ollama references are rejected as ordinary local PEFT training inputs. GPU/quantization behavior is dependency- and hardware-dependent.

Jobs are persisted at submission, terminal completion, cancellation and shutdown; live metrics primarily mutate the in-memory job object. On startup, persisted `queued` or `running` records become `error` with an interruption message. Jobs are not automatically resumed, and intermediate metric updates are not continuously checkpointed. Trainer checkpoint saving is disabled in this workflow.

Cancellation of a queued job is immediate in its status. Running cancellation is cooperative and may wait for a model load or training step. Dataset deletion has no equivalent active-job reference guard, so callers should not remove an input needed by queued/running work.

## Adapter lifecycle

Adapter metadata in [`AdapterStore`](backend/src/backend/adapters/store.py) includes base-model reference, architecture, rank, alpha, requested target modules, LoRA/QLoRA method, quantization, weight path, training job ID, timestamps and status.

Activation through `/inference/adapter` checks the record/files and PEFT dependencies, resolves the base model, unloads the current engine, and creates a new `TorchPeftEngine(base, adapter_path)`. This is a model/engine reload, not an in-place multi-adapter hot swap. Clearing the adapter unloads that engine; it does not retain a plain base model in memory. Deleting the active adapter also unloads it first.

The adapter router packages exports as ZIP archives containing `record.json` and `weights/`. It returns a download path, which the desktop saves using the atomic download helper. Import reads a local ZIP and validates metadata, IDs, duplicate entries, traversal/absolute paths, symlinks, PEFT configuration, and nonempty weight files. Limits include 1 MiB metadata/configuration files, 10,000 archive entries, and 8 GiB aggregate uncompressed member size. Imports are serialized and duplicate adapter IDs are rejected.

Imported files go into a fresh directory under `adapters/imports/`; the stored path is replaced with that local destination. Adapter deletion requires a path inside the managed adapter root and refuses shared root/import/export directories. Export archives are separate artifacts and remain independently managed.

Training output directories currently derive from the configured adapter name. Unique adapter record IDs therefore do not themselves guarantee unique weight directories for repeated names. Native multi-LoRA serving and conversion of PEFT adapters to GGUF are not implemented.

## Retrieval and document processing

RAG is a local pipeline implemented by extraction/chunking helpers, a cached sentence-transformers embedder, and [`RagStore`](backend/src/backend/rag/store.py).

### Ingestion

1. Accept a local document path or named inline content.
2. For paths, extract text from TXT, Markdown, HTML or PDF. Markdown processing retains code content; HTML stripping removes scripts/styles/tags; PDFs use `pypdf` text extraction without OCR. Inline content is used directly as text.
3. Split into overlapping whitespace-separated word chunks. Defaults are 512 words with 64-word overlap; schema fields and stored counts sometimes call these “tokens,” but they are not tokenizer tokens.
4. Lazily load the configured sentence-transformers model, defaulting to `all-MiniLM-L6-v2`, and encode normalized float32 vectors.
5. Check cancellation and store document metadata, chunk text and vectors in one SQLite transaction.

The document index lives in `rag/rag.db`, with `rag_documents`, `rag_chunks`, and `rag_config` tables. Vectors are float32 BLOBs. Original documents are not copied into a separate managed document repository; extracted chunks are persisted and source paths are recorded.

### Retrieval and chat augmentation

A query is embedded with the configured model. `RagStore.search()` reads compatible chunks from SQLite, stacks their vectors into a NumPy matrix, computes dot products, sorts by similarity, applies the threshold, and returns the top results. L2 normalization makes the dot product equivalent to cosine similarity. Defaults are `top_k=4` and threshold `0`, with zero disabling score filtering.

There is no FAISS dependency, vector server, approximate-nearest-neighbor index, or persistent in-memory search index. Each search reconstructs the matrix and performs a full scan plus sorting. This suits modest local collections; cost and memory grow with chunk count and embedding dimension.

For RAG-enabled chat, the latest user message becomes the retrieval query. Retrieved passages are numbered and labeled with document names, assembled into a system message, and prepended to conversation history. A binary search truncates the context string to fit the engine's remaining input budget. This can cut a passage partway through; it is not a passage-aware packing algorithm.

Missing dependencies, an empty index, no matching passages, or insufficient context space produce errors instead of silently generating without retrieval. Chat SSE contains text deltas, not a separate structured citation payload; explicit `/rag/query` results provide source/chunk metadata.

Configuration is merged and validated on update. Changing the embedding model is blocked while incompatible documents remain; users must clear and re-ingest. Ingestion checks for model changes and embedding-dimension mismatches before commit. Deleting indexed documents removes chunks and metadata but preserves original files.

## Persistence and configuration

The application data root comes from `MAGNITE_HOME`. Otherwise it is `~/.magnite`; Windows has an `%APPDATA%/Magnite` branch. The standalone CLI `--home` sets `MAGNITE_HOME` before service startup.

```text
<application home>/
  models.db                 model registry
  datasets.db               dataset metadata
  training_jobs.db          serialized job records in a jobs table
  adapters.db               adapter metadata
  datasets/
    <dataset-id>.jsonl      normalized training samples
  adapters/
    <adapter-name>/         trained PEFT weights and optional tokenizer files
    _work/                 Trainer output location
    imports/               extracted imported adapters
    exports/               generated ZIP archives
  rag/
    rag.db                 document metadata, chunks, embeddings, configuration
  vllm.log                  vLLM stderr log when that engine is used
```

SQLite stores use ordinary connections and schema creation with `CREATE TABLE IF NOT EXISTS`; there is no versioned migration framework. Metadata links across the separate databases are application-level references, not cross-database foreign keys or transactions. Filesystem writes and metadata writes are also not one atomic transaction across every workflow.

| State | Persistence and ownership |
| --- | --- |
| Models/datasets/adapters/RAG | SQLite records plus referenced files |
| Training history | SQLite serialized job snapshots; live worker/queue in memory |
| Pull jobs | Backend memory only |
| Loaded inference engine and active adapter | Backend memory; not automatically restored |
| Conversation history and partial responses | Desktop memory only |
| Panel options | JSON values under QSettings `panels/<panel title>` |
| Engine/data-directory/appearance preferences | QSettings keys such as `engine/override`, `storage/dataDir`, and `appearance/studio3*` |
| Model and embedding weights | Library-managed caches or Ollama-owned storage, generally outside application home |

`HF_HOME` is passed directly as the registry's `snapshot_download(cache_dir=...)` value. Other Hugging Face/Transformers/sentence-transformers consumers use their library/environment defaults. Setting `MAGNITE_HOME` does not move all model caches.

Saving a data-directory preference applies at the next desktop backend launch and does not migrate existing files. Environment `MAGNITE_HOME` takes precedence. Engine preference changes are also posted to `/inference/settings`, affecting subsequent engine selection without automatically reloading the active model.

Records can contain absolute paths. Backup and relocation must account for application data, Qt settings, external weights, and original input files. See [storage and privacy](docs/STORAGE_AND_PRIVACY.md) for the supported manual backup procedure.

## Concurrency and failure behavior

| Mechanism | Guarantee and boundary |
| --- | --- |
| Supervisor nonblocking `threading.Lock` | One inference lifecycle/generation operation at a time; competing operations fail as busy rather than queue |
| Training queue + one daemon worker | Serial training jobs within one backend; independent of inference resource use |
| Per-pull daemon threads | Downloads run off request threads; jobs have no durable queue/cancellation API |
| Store locks + SQLite transactions | Protect individual store operations; locks do not span every store instance or the entire application |
| `request_work()` | Runs long operations in a thread, monitors HTTP disconnects, signals cancellation, retains ownership until native work exits |
| `threaded_stream()` / `run_blocking()` | Bridge blocking ML calls to async iteration and keep engine ownership during cleanup |
| Qt network callbacks/timers | UI remains asynchronous during backend operations; startup/shutdown process waits can still block |

The inference lock covers preparation, generation and cleanup. Closing a stream sets a stop event and waits for its worker to exit before unloading is allowed. Torch adds stopping criteria and joins its generation thread. The response wrapper also closes generators if ASGI sending fails.

Disconnect-aware model loads discard/unload a completed result when the request has been cancelled. Document ingestion checks cancellation before registration. A disconnected operation can return internal HTTP status 499 where a response is still possible. Native calls cannot generally be interrupted immediately, so cancellation is not equivalent to instantaneous resource release.

HTTP validation errors, missing records, busy conflicts and execution errors are returned through route-specific mappings. Once streaming headers are committed, errors are communicated inside SSE. A network timeout alone does not prove server-side work stopped; the desktop's transport error advises refreshing state before retrying.

## Trust and network boundaries

The backend binds to `127.0.0.1` and has no application authentication layer. It runs with the user's filesystem permissions and can ingest local paths. This is a trusted local desktop service, not a public/LAN deployment architecture or an isolation boundary between untrusted local users/processes.

Network activity includes Hugging Face search/download/metadata access, initial embedding-weight downloads, and communication with Ollama. `MAGNITE_OLLAMA_URL` defaults to `http://127.0.0.1:11434` but can point elsewhere, changing where Ollama inference and prompts go. vLLM is started locally. No application telemetry or encryption-at-rest feature is implemented.

Archive validation and managed deletion checks protect specific file operations; they do not sandbox ML dependencies, training, or the backend generally. Exports and logs can include local paths, model references and error details. Offline operation requires appropriate cached weights and dependencies, not merely a local GUI/backend connection.

## Build and verification

The native build requires CMake 3.20+, C++20, and Qt 6 Widgets, Network and Svg. CMake enables AUTOMOC/AUTORCC/AUTOUIC and `-Wall -Wextra` for GCC/Clang. New C++ sources must be added to `MAGNITE_SOURCES` in `app/CMakeLists.txt`.

The Python package requires Python 3.10+. Base dependencies include FastAPI, Uvicorn, HTTPX, Pydantic, huggingface_hub, llama-cpp-python, tqdm, AnyIO and NumPy. The `[ml]` extra adds torch, Transformers, PEFT, bitsandbytes, sentence-transformers, datasets, accelerate, safetensors and pypdf. vLLM and Ollama require separate installation. Dependencies have lower bounds rather than a fully locked environment.

```bash
python3 -m venv .venv
.venv/bin/python -m pip install -e 'backend[ml]'
cmake -S app -B build
cmake --build build -j2
./build/magnite-app
```

Use `-e backend` for the base GGUF inference installation without training/RAG extras. Keep the executable in the expected `build/` location for backend discovery. Standalone service startup is available through `magnite-backend --port 0 --home <directory>`.

Verification layers are complementary:

| Layer | Entry point | Scope |
| --- | --- | --- |
| Offline backend regressions | `PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests -v` | API validation, archives, stores, job history/cancellation, streaming cleanup, mocked engine behavior |
| Theme audit | `./build/magnite-app --theme-check` | Tokens, stylesheet/resource/font/icon checks |
| UI checks | `./build/magnite-app --ui-check` | Interaction and layout checks |
| Bare desktop selftest | `./build/magnite-app --selftest` | Desktop/IPC integration and UI/error behaviors |
| Opt-in model workflows | `MAGNITE_SELFTEST_PULL=1`, `MAGNITE_SELFTEST_INFERENCE=1`, `MAGNITE_SELFTEST_TRAINING=1`, `MAGNITE_SELFTEST_ADAPTER=1`, or `MAGNITE_SELFTEST_RAG=1`, with `--selftest` | Download/load/chat/train/activate/retrieve workflows; may download toy weights |
| Visual captures | `./build/magnite-app --screenshot <dir>` | Panel screenshots for manual review |
| Additional live checks | [`backend/tests/live_workflow.py`](backend/tests/live_workflow.py) | Real-service workflow verification |

The [CI workflow](.github/workflows/verify.yml) configures Ubuntu, Python 3.12, Qt offscreen mode, temporary application/settings storage, a base backend installation, backend regressions, native build, theme audit, UI checks and bare selftest. Its configuration is not evidence that a particular hosted run passed. Live vLLM/Ollama, platform packaging and hardware combinations require separate verification; see the [verification report](docs/release/VERIFICATION.md) and [release checklist](RELEASE_CHECKLIST.md).

## Extension points and constraints

For a new inference engine, implement `BaseEngine`, register availability/construction/selection in the supervisor, extend load-request validation and the UI engine choices, and verify context accounting plus cancellation before unload. Keep heavy imports lazy and retain the single-operation ownership contract.

For an API change, update the relevant route and Pydantic schemas, service behavior, C++ request/response handling, offline regressions and [LOCAL_API.md](docs/LOCAL_API.md). There is no shared generated contract to catch all cross-language mismatches automatically.

For a new workspace, derive from `BasePanel`, add navigation and backend wiring in `MainWindow`, add explicit CMake sources, and use the token/QSS/icon system. Persist preferences through the existing settings pathway; durable user content should have an explicitly designed store rather than being placed implicitly in panel settings.

For retrieval changes, preserve embedding-model/dimension consistency and atomic document/chunk insertion. Replacing the flat search with a scalable index would also require a durable synchronization/rebuild strategy, not just a different similarity function.

Current architectural limits include single-engine inference, cooperative cancellation, no global GPU scheduling, no durable chat, no automatic training resume, no automatic schema/path migration, flat full-scan retrieval, and source-checkout-dependent desktop deployment. Tiny-model tests establish workflow behavior rather than useful model quality, training effectiveness, or large-model reliability. These limits are documented further in [LIMITATIONS.md](docs/LIMITATIONS.md).
