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

When RAG supplies passages, a `sources` event precedes the reply deltas.
It contains an array of passages with `document_name`, a zero-based chunk
`index`, and the `text` included in the model context. Chat `rag.document_ids`
limits retrieval to the listed sources. Omitting it searches all indexed documents.

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

## Blueprints

Blueprint runs use deterministic transforms and dataset snapshots. Select
`mode: "local"` for Torch/PEFT generation, or `mode: "fixture"` to replay
provided responses. Existing requests default to fixture mode.

| Operation | Route |
| --- | --- |
| List saved blueprints | `GET /blueprints` with optional `limit` and `offset` |
| Save a new blueprint or add a revision | `POST /blueprints` or `PUT /blueprints/{id}` with the document as JSON |
| Delete a blueprint | `DELETE /blueprints/{id}?expected_revision=N`. Existing run snapshots and approval records remain available. |
| Read a saved blueprint | `GET /blueprints/{id}` with optional `revision`. The default is the latest revision. |
| Check graph and local bindings | `POST /blueprints/validate` with a document. The result has `valid` and `issues`. |
| Check local runtime support | `POST /blueprints/capabilities` with a document. Reports device, estimated memory, context and thinking support by model node. |
| List local checkpoint options | `GET /blueprints/checkpoints` lists eligible local Transformers checkpoints. `GET /blueprints/checkpoints/{model_id}` checks context size, thinking support, and compatible adapters for one checkpoint. |
| Export or import the portable document | `GET /blueprints/{id}/export` with optional `revision`, or `POST /blueprints/import` with the exported JSON |
| Load the editable order starter | `GET /blueprints/starters/order` with an optional registered `model_id`. Returns the document, fixture samples, and training examples for each adapter. |
| Train an adapter for a node | `POST /blueprints/{id}/nodes/{node_id}/training` with typed `examples`, optional `revision`, and optional training `settings`. Returns an ordinary training job. |
| Bind an existing or trained adapter | `PUT /blueprints/{id}/nodes/{node_id}/adapter` with the current `revision` and either `adapter_id` or a completed `training_job_id` for that node. Returns a new revision. |
| Queue a run | `POST /blueprints/{id}/runs` with `inputs`, optional `revision`, and `mode`. Fixture mode takes `fixtures` for Model, Reasoning and Tool nodes. Use the query `surface=chat` for a saved workflow launched in chat. Returns HTTP 202 with a run ID. |
| Queue an isolated node test | `POST /blueprints/{id}/node-tests` with the blueprint `name`, the full `node` contract, and named `inputs`. It runs locally without other nodes and returns HTTP 202. An Input node takes its test value under `inputs.value`. |
| Read history or details | `GET /blueprints/runs` with optional `blueprint_id`, `surface`, `active`, `limit`, and `offset`. `GET /blueprints/runs/{run_id}` includes the snapshot, per-step trace, and results. |
| Follow or replay events | `GET /blueprints/runs/{run_id}/events` |
| Cancel a run | `POST /blueprints/runs/{run_id}/cancel` returns the persisted run status. Repeating the request is safe. |

The desktop keeps incomplete graphs as local drafts. To publish one by a
client-generated ID, use `PUT /blueprints/{id}?expected_revision=0` with a
32-character lowercase hexadecimal ID. For edits, pass the revision the draft
was based on. Repeating the same save after a response is lost returns the
already-saved revision; a competing edit returns HTTP 409.

A node test is available before publication. Its run includes `test_node`,
and uses revision 0 when the tested node does not match the current saved
revision. Its result maps the tested node ID to its named outputs. A test of a
Tool node requires the same authenticated launch and approval broker as a full
local run.

A portable document has `version: 1`, a name, nodes, and edges. Each edge connects
`source: {"node": "...", "port": "..."}` to a corresponding `target`. Port
names are keys in the node's `inputs` and `outputs` maps. Edges must form a DAG
and each input port must have one source. Declared types must agree on connected
ports and shared nested fields, with integer outputs accepted by number inputs.
The runner validates constraints on both sides as values pass through the graph.
Schemas support `string`, `integer`, `number`, `boolean`, `object`, `array`,
and `null`, with `minLength`, `minimum`, `items`, `properties`, `required`,
and `additionalProperties` on the applicable types.

| Node kind / operation | Inputs → outputs |
| --- | --- |
| `input` / `input` | Run request `inputs[node_id]` → `value` |
| `dataset` / `dataset-rows` | Inline `binding: {"source":"inline","rows":[...]}` or registered `binding: {"source":"local","dataset_id":"..."}` → `rows` with an array schema and a declared `items` schema |
| `transform` / `pass`, `parse-json`, `field`, `first`, `count`, `format` | `value` → `value`. `field` takes `parameters: {"field":"..."}`. |
| `transform` / `order-clause`, `order-price` | `message` and one-based `index` → selected clause. `clause` and extracted `fields` → validated order with prices. Both return through `value`. |
| `model` / `generate`, `reasoning` / `reason` | Declared input ports → declared output ports. Both bind `model_id` and optional `adapter_id`, and accept `system_prompt`, `response`, and `max_tokens`. |
| `tool` / `approval` | In local mode, an action object on `value` requests approval and produces `receipt` after dispatch. In fixture mode, `receipt` uses the supplied decision. |
| `tool` / `agent` | Text or a chat message array on `value` starts Pi. `receipt` contains the final `text`. |
| `output` / `output` | `value` → `value` and run `results[node_id]` |

`parse-json` takes a string, `field` takes an object, and `first` and `count`
take arrays. `count` returns an integer and `format` returns a string.

Save requires a structurally valid graph. Validate, import, and run admission
also resolve registered datasets, local Transformers checkpoints, and compatible
PEFT adapters. Change registry IDs in the document before importing it into a
profile with different IDs. Import assigns a fresh blueprint ID and revision 1.
Graph or binding errors include a code and, where applicable, a node ID and path.

Local execution uses a registered, unquantized Transformers text checkpoint
with local Safetensors weights and tokenizer files. LoRA adapters must match
that checkpoint and support switching without shared biases. A local run holds
one Blueprint checkpoint at a time. Model loading, adapter selection, and
generation share the inference lock with chat. A loaded chat model contributes
to the machine's memory usage. Checkpoint estimates include runtime overhead and
the output token budget. The actual prompt and output must fit the checkpoint's
context window.

The system prompt and port values form the model prompt. One input is sent as
its value. Multiple inputs are sent as a JSON object keyed by port. With the
default `response: "json"`, one output uses the model's whole JSON value and
multiple outputs require a JSON object keyed by port. `response: "text"`
requires one string port. A Reasoning node can reference a separate checkpoint.
Its chat template needs an `enable_thinking` control and a `</think>` boundary.
Its token budget covers thinking and the final answer, and the runner validates
the final answer against its output ports.

To try the starter, save the returned `blueprint` with `POST /blueprints` and
submit a `samples[].request` to its run route. For real generation, bind adapters
for `select` and `extract` and submit the sample's `inputs` with `mode: "local"`.
To train those adapters, send the corresponding `training_examples` array to the
node's training route. Each example contains `inputs` and `outputs` maps keyed by
runtime ports. Training uses the node's prompt and learns the provided final answer.
After the job finishes, use its ID and the current revision on the adapter
route. An existing compatible adapter can be bound by ID.

A fixture has the shape `fixtures: {"node_id":{"outputs":{"port":"value"}}}`.
The `outputs` map uses the node's declared output ports. An optional `delay_ms`
from 0 to 5000 simulates a slow step. A run reports `mode: "fixture"`.
The executor supports up to 64 nodes
and 512 edges per document, 16 MiB of linked dataset files per run, and 32 MiB of
recorded step inputs and outputs per run.

The SSE stream emits named `run` and `node` events with an `id` and JSON `data`.
Pass the last received ID in `Last-Event-ID` or `?after=` to resume. The stream
closes after replaying the terminal event. Runs end as `passed`, `failed`, or
`cancelled`. Inspect the run detail for values and validation errors. Closing
the event stream leaves the run active. An interrupted run is marked failed on
startup with an `interrupted` error.

[`blueprints.db` storage and compatibility](development/BACKEND_PROCESS.md#9-storage-and-environment-reference)

## Tool chat and approvals

External actions require the authenticated desktop launch. To try the routes in
a standalone backend, start it with `MAGNITE_API_TOKEN` and supply the same
Bearer credential on requests. Pi uses the already loaded Torch, llama.cpp, or
local vLLM chat engine. Install the locked Pi dependencies with Node 22.19 or later.
[Pi integration and setup](development/blueprints/PI_INTEGRATION.md)

| Operation | Route |
| --- | --- |
| Check Pi and the loaded model | `GET /inference/tool-runs/capabilities` reports availability and engine |
| Start tool chat | `POST /inference/tool-runs` with `messages` and optional absolute `working_directory` |
| Inspect pending actions or review audit history | `GET /blueprints/runs/{run_id}/approvals` |
| Decide once | `POST /blueprints/runs/{run_id}/approvals/{approval_id}/decision` with the displayed `fingerprint` and `decision` of `approve_once` or `deny` |

Tool chat returns a normal run ID with `surface: "chat"`. Follow, reconnect,
or cancel using the Blueprint run routes above. Read the completed answer at
`results.reply.text`. The usual `/inference/chat` endpoint continues to
serve ordinary chat.

A local Blueprint can pass `{"action":"open","target":"..."}` or
`{"action":"command","executable":"...","arguments":["..."]}` to its
`approval` node. The run request may specify an absolute `working_directory`
for either surface. Its default is `tool-workspace/` under `MAGNITE_HOME`.
Open accepts a regular file, folder, or HTTP(S) URL. Commands run directly with
the displayed executable, argument array, and working directory. They return
an outcome code. Open reports dispatch to the operating system.

SSE emits `approval` events containing the proposal ID, status, and
fingerprint. Fetch approval details to show the exact pending action. The
approval expires after 90 seconds. Display the normalized target or each
argument and the working directory before issuing a decision. A stale or
duplicate decision returns `409`. Another run's approval ID returns `404`.
Pending approvals close with the run on cancellation or interruption.
The event and audit history retain identifiers, kind, timing, and outcome.
Exact pending action details are available while the approval awaits a decision.

## Other route groups

| Operation | Route / body |
| --- | --- |
| Hub search | `GET /models/search?q=...&limit=50&offset=0` (`limit` 1-100, `offset` 0-1000; each result has `downloaded`; 502 if the Hub is unreachable) |
| Hub download sizes | `GET /models/sizes?repo=owner/name&repo=...` (1-100 repos; exact bytes a download fetches, i.e. every file in the repo, or `null` if the Hub cannot say; never an error because the Hub is down) |
| Pull model / poll / cancel | `POST /models/pull` with `source`, `ref` (409 if another download of that source is running; asking for the running one returns its job); `GET /models/pull/{job_id}` (`status`: queued, running, done, error, cancelled; `bytes_done`, `bytes_total`, `progress`); `POST /models/pull/{job_id}/cancel` (202 while stopping, 200 if already finished) |
| Discover Hub models | `GET /models/discover?q=&limit=30&offset=0`; download-sorted results with optional task, architecture, language, format, parameter and context metadata |
| Hub model details | `GET /models/hub-detail?repo=ORG/NAME`; revision, bounded file inventory, exact repository bytes when all file sizes are known, configuration and publisher evaluation metadata |
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
| Retrieve | `POST /rag/query` with `query`, optional `top_k`, `min_score`, `document_ids` |
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

The native Model Hub filters and sorts the loaded results. Its download action fetches the complete repository using the existing pull API. Metadata detail requests may cache a bounded `config.json` pinned to the returned revision. Missing metadata stays unknown.

`POST /models/import` registers a local GGUF file and returns its model record
with status 201. Send `{"path":"/absolute/path/model.gguf"}`. The registry
references the original file in place. Deleting its record leaves that file
on disk. Load the returned `local:` ID using Auto or llama.cpp.
