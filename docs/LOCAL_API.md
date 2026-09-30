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

The examples contain deliberate placeholders for values from your own backend.
Do not expose this service through port forwarding or a public proxy: it has no
application authentication and provides local-path and mutation operations.

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
| Hub search | `GET /models/search?q=...` |
| Pull model / poll | `POST /models/pull` with `source`, `ref`; `GET /models/pull/{job_id}` |
| Delete model | `DELETE /models/{model_id}` |
| Ingest dataset | `POST /datasets/ingest` with absolute `path`, or `name`, `format`, `content` |
| List/read/delete datasets | `GET /datasets`, `GET /datasets/{id}`, `DELETE /datasets/{id}` |
| Start training | `POST /training/start` with `base_model`, `dataset_id`, training settings |
| Training history/status/cancel | `GET /training/jobs`, `GET /training/status/{id}`, `POST /training/cancel/{id}` |
| List adapters | `GET /adapters` |
| Activate / clear adapter | `POST /inference/adapter` with `adapter_id`; empty string clears |
| Export / download | `POST /adapters/export/{id}`, then GET the returned `download_path` |
| Import / delete | `POST /adapters/import` with local ZIP `path`; `DELETE /adapters/{id}` |
| RAG status/config | `GET /rag/status`, `GET /rag/config`, `POST /rag/config` with partial settings |
| Ingest/list documents | `POST /rag/documents` with `path` or `name`/`content`; `GET /rag/documents` |
| Retrieve | `POST /rag/query` with `query`, optional `top_k`, `min_score` |
| Delete/clear index | `DELETE /rag/documents/{id}`, `POST /rag/clear` |
| Runtime engine preference | `POST /inference/settings` with `engine_override`: 0 Auto, 1 vLLM, 2 llama.cpp |

Training example body after dataset ingestion:

```json
{"base_model":"REGISTERED_MODEL_ID","dataset_id":"DATASET_ID","name":"demo","rank":2,"alpha":4,"target_modules":"all-linear","quantization":"none","max_steps":3,"max_length":64}
```

These APIs accept server-local paths, not multipart browser uploads. Use paths
accessible to the backend process. Check HTTP status and returned details rather
than treating a completed request as successful work.
