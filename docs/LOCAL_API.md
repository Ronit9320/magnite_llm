# Local backend API

The desktop starts FastAPI on a random `127.0.0.1` port. For manual experiments,
start a separate disposable backend as shown in [installation](INSTALL.md), copy
its printed port, and set `MAGNITE_API` below. Routes and Pydantic schemas are the
contract; the running backend exposes `/docs` and `/openapi.json`.

```bash
export MAGNITE_API=http://127.0.0.1:REPLACE_WITH_PRINTED_PORT
curl --fail-with-body "$MAGNITE_API/health"
curl --fail-with-body "$MAGNITE_API/models"
```

`GET /models` refreshes existing Hugging Face cache snapshots and Ollama models.
Use `?source=huggingface` for offline Hub-cache discovery without an Ollama probe.
See [cache discovery rules and paths](development/BACKEND_PROCESS.md#4-model-library-discovery-pulls-and-deletion).

The examples contain deliberate placeholders for values from your own backend.
Do not expose this service through port forwarding or a public proxy: it provides
local-path and mutation operations. A backend started by hand, as above, has no
credential check; one launched by the desktop app requires a per-launch secret
(`MAGNITE_API_TOKEN`) on every request except `/health`, so these examples do not
work against the app's own backend. Requests with a non-loopback `Host` are refused
in both cases. Linux integration checks cover authenticated API access and updater coordination;
macOS results for earlier builds are recorded separately.

## Inference

Load a registered model ID (from `/models`) or a supported model reference:

```bash
curl --fail-with-body "$MAGNITE_API/inference/load"   -H 'Content-Type: application/json'   -d '{"model":"REPLACE_WITH_REGISTERED_MODEL_ID"}'
curl --fail-with-body "$MAGNITE_API/inference/status"
curl --fail-with-body -N "$MAGNITE_API/inference/chat"   -H 'Content-Type: application/json'   -d '{"messages":[{"role":"user","content":"Hello"}],"max_tokens":64,"stream":true}'
```

An explicit load `engine` may be `llamacpp`, `torch`, `vllm`, or `ollama`.
Inference uses the already loaded model; a `model` field in a chat request does
not load or switch models.

Stream frames use SSE `data:` lines with JSON objects, separated by blank lines:

```text
data: {"delta": "Hello"}

data: {"done": true}

```

The terminal event can instead be `{"error":"message"}`. A stream that ends
without `done` or `error` is incomplete. Startup errors may return an ordinary
non-2xx HTTP response before streaming starts. Clients must handle both.

`stream:false` returns JSON with `choices[0].message.content` for chat.
`POST /inference/completion` accepts `prompt`, `max_tokens`, `temperature`, and
`stream`; non-streaming completion returns `choices[0].text`. The response shapes
are OpenAI-like, but routes and SSE frames are not a drop-in OpenAI API.

```bash
curl --fail-with-body "$MAGNITE_API/inference/chat"   -H 'Content-Type: application/json'   -d '{"messages":[{"role":"user","content":"Summarize my documents"}],"rag":{"enabled":true,"top_k":4},"max_tokens":64,"stream":false}'
curl --fail-with-body -X POST "$MAGNITE_API/inference/unload"
```

RAG requires indexed documents and enough remaining context. Loading/unloading
and adapter changes can be rejected while generation owns the model. Closing a
stream requests cleanup; native work may stop only after yielding or returning.

## Other route groups

| Operation | Route / body |
| --- | --- |
| Hub search | `GET /models/search?q=...&limit=50&offset=0` (`limit` 1-100, `offset` 0-1000; each result has `downloaded`; 502 if the Hub is unreachable) |
| Hub download sizes | `GET /models/sizes?repo=owner/name&repo=...` (1-100 repos; exact bytes a download fetches, i.e. every file in the repo, or `null` if the Hub cannot say; never an error because the Hub is down) |
| Pull model / poll / cancel | `POST /models/pull` with `source`, `ref` (409 if another download of that source is running; asking for the running one returns its job); `GET /models/pull/{job_id}` (`status`: queued, running, done, error, cancelled; `bytes_done`, `bytes_total`, `progress`); `POST /models/pull/{job_id}/cancel` (202 while stopping, 200 if already finished) |
| Delete model | `DELETE /models/{model_id}` |
| Ingest dataset | `POST /datasets/ingest` with absolute `path`, or `name`, `format`, `content` |
| List/read/delete datasets | `GET /datasets`, `GET /datasets/{id}`, `DELETE /datasets/{id}` |
| Start training | `POST /training/start` with `base_model`, `dataset_id`, training settings |
| Training resource estimate | `POST /training/estimate` with training settings; advisory local model/memory budget and measured-history duration calibration |
| Training capability probe | `GET /training/capabilities`; reports quantization availability and platform reason |
| Training history/status/cancel | `GET /training/jobs`, `GET /training/status/{id}`, `POST /training/cancel/{id}` |
| List adapters | `GET /adapters` |
| Activate / clear adapter | `POST /inference/adapter` with `adapter_id`; empty string clears |
| Export / download | `POST /adapters/export/{id}`, then GET the returned `download_path` |
| Import / delete | `POST /adapters/import` with local ZIP `path`; `DELETE /adapters/{id}` |
| RAG status/config | `GET /rag/status`, `GET /rag/config`, `POST /rag/config` with partial settings |
| Ingest/list documents | `POST /rag/documents` with `path` or `name`/`content`; `GET /rag/documents` |
| Retrieve | `POST /rag/query` with `query`, optional `top_k`, `min_score` |
| Delete/clear index | `DELETE /rag/documents/{id}`, `POST /rag/clear` |
| Context usage | `POST /inference/context-usage` with `messages`, `max_tokens`; returns `used`, `reserved`, `limit`, `fill`, `exact` (409 when no model is loaded; works during generation) |
| Compact a long chat | `POST /inference/compact` with `messages`, optional `previous_summary`, `max_tokens`, `target_fill`, `keep_recent`; returns `summary`, `covered`, `method` (409 while busy) |
| Runtime engine preference | `POST /inference/settings` with `engine_override`: 0 Auto, 1 vLLM, 2 llama.cpp |
| Prepare application update | `POST /updates/quiesce` with `{}`; 200 when idle, 409 while work is active |
| Desktop graceful shutdown | `POST /_desktop/shutdown`; requires a configured launch token; absent in tokenless developer mode |
| Cancel update maintenance | `POST /updates/resume` with `{}`; 200 restores request admission |

Update maintenance is implemented by ASGI middleware. Quiescing is refused while
HTTP responses/worker cleanup, training jobs or model pulls are active. Once
quiesced, new application requests return 503; `/health` and the maintenance
controls remain available. The desktop stops its backend before replacing the
application bundle. These endpoints coordinate lifecycle and do not implement
authentication themselves; the CLI access wrapper protects them when a token is
configured. Direct `uvicorn backend.server:app` bypasses that wrapper and the
data-directory lock. See [update architecture](development/RELEASE_PIPELINE.md).

Dataset ingestion accepts JSONL, CSV and plain text and stores normalized JSONL.
JSONL supports `messages`, `text`, `instruction`/`output` (with optional `input`),
`prompt`/`completion`, and `prompt`/`response` rows. A `prompt`/`response` pair
becomes user/assistant chat messages; both fields must be nonempty strings.
CSV files with `prompt,response` headers use the same chat normalization.

Training example body after dataset ingestion:

```json
{"base_model":"REGISTERED_MODEL_ID","dataset_id":"DATASET_ID","name":"demo","rank":2,"alpha":4,"target_modules":"all-linear","quantization":"none","max_steps":3,"max_length":64}
```

Training settings also accept `dropout` (default `0.05`, `0 <= x < 1`) and
`validation_split` (default `0`, range `0..0.5`). A nonzero validation split
requires at least two rows. Held-out rows are excluded from training and
`eval_loss` is logged at each epoch boundary. `TrainingJob.history` retains the
latest 1,000 metric entries with actual `step`, monotonic `elapsed_seconds`,
optional `epoch`, and the finite values present in each log: `loss`, `eval_loss`,
`learning_rate`, `grad_norm`. Missing metrics are omitted, not set to zero.
Existing jobs without these optional fields remain readable. See
[training visualisation](development/TRAINING_VISUALISATION.md) for UI behavior,
timing estimates and verification.

These APIs accept server-local paths, not multipart browser uploads. Use paths
accessible to the backend process. Check HTTP status and returned details rather
than treating a completed request as successful work.

### Import and lifecycle validation

- Deleting a dataset used by queued or running training returns 409.
- Dataset CSV fields allow up to 131,072 characters; malformed/oversized fields
  and invalid sample values return 400 with row details. UTF-8 BOMs are accepted.
- Adapter imports require valid LoRA configuration and safetensors weights.
  Pickle weights are unsupported. Missing/corrupt source files make export fail;
  successful exports each have their own immutable download URL.
- Optional inference `model` must match the loaded model (registered IDs resolve
  to their local snapshot); mismatches return 400 before generation begins.
  Disconnect monitoring applies to both streaming and non-streaming requests.
- Raw HTML/Markdown uses the same text extraction as file ingestion. PDFs require
  a file path; corrupt, encrypted, or textless PDFs return 400.
- Training cancellation remains available during saving. Cancellation accepted
  before adapter publication discards the saved output; after publication the
  job is terminal and cancellation is rejected.

### `GET /training/telemetry`

Lightweight device-wide compute readings, with no ML runtime imports. Returns
`device`, `accelerator`, `kind`, `used_bytes`, `total_bytes`,
`utilization_percent`, `temperature_c`, `power_w` and `detail`.
An available NVIDIA probe describes one GPU; memory is never summed across GPUs.
If GPU telemetry is unavailable, the response labels system/unified memory and
leaves unsupported scalars `null`. These are device-wide readings, not an
assertion that the selected training job uses that GPU. The desktop refreshes
this card while its training page is visible, with one request in flight.

### Settings and local diagnostics

These read-only endpoints use the desktop launch credential like other routes
(except `/health`). They are also covered by update quiescing.

- `GET /system/info`: backend version, OS/architecture, Python version, installed
  inference/training package versions and device telemetry. Missing packages and
  measurements are `null`; package inspection does not import ML runtimes.
  Installed packages do not guarantee hardware/model compatibility.
- `GET /system/storage`: resolved `data_directory` and Hugging Face `model_cache`,
  `workspace` and `models` objects (`bytes`, `complete`), `free_bytes` for the data
  volume, and `combined_locations`. Scans are bounded by time and entry count,
  exclude symbolic links and avoid counting nested locations twice. Incomplete
  scans are lower bounds. Cache storage is shared and may be outside app home.
- `GET /system/logs`: a bounded 64 KiB tail of the local vLLM log, with common
  credential patterns redacted, plus `truncated`. No arbitrary path is accepted.
  Logs can contain local file paths/model names and are never part of the default
  diagnostic report. A missing log is represented by explanatory text.

Chat export/deletion are native operations over the existing chat store, rather
than new backend routes. Export format `magnite-chat-export` version 1 contains
an `exported_at` timestamp and a `chats` array of complete existing chat records,
including branch trees. Export writes atomically and fails if a listed chat is
unreadable. It is a portable export, not an automatic backup/import feature.
