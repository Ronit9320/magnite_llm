# Magnite extensive bug-testing report

Tested **2026-10-09**, source commit **6c20082f319569b3e9c5388d05d5388bd59fb799**.

**13 reproducible findings: 3 high, 10 medium.** No application fixes were made. This report, a reusable probe script, and evidence logs are the audit deliverables. Existing tests passing does not cover the failure cases below.

## Environment and scope

- Linux x86-64, kernel 7.2.9-arch1-1, glibc 2.44; Python 3.12.12; Qt6 desktop built from this checkout.
- FastAPI 0.141.1, Pydantic 2.13.5, HTTPX 0.28.1, torch 2.14.0, transformers 5.16.1, PEFT 0.20.0, llama-cpp-python 0.3.35, sentence-transformers 6.0.1, pypdf 6.19.0.
- Fresh temporary `MAGNITE_HOME` and settings directories for new tests. Only disposable datasets/adapters were deleted. Tiny model tests were allowed to populate model caches.
- Backend API tests, parser boundaries, deterministic lifecycle races, concurrent storage operations, actual tiny-model inference/training/RAG, UI interaction/layout audits at normal and 200% scaling, desktop/backend startup/shutdown, and the existing AppImage.
- Severity: **high** = lost or silently replaced data / interrupted active work; **medium** = broken functionality, incorrect results, unusable accepted artifacts, or resource/cancellation failure.

## Results and coverage

| Test                               | Result                     | Evidence / qualification                                                                                                                                                                                           |
| ---------------------------------- | -------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| CMake configure and build          | PASS                       | `cmake -S app -B build && cmake --build build -j 2`                                                                                                                                                                |
| Existing offline regressions       | PASS, 41 tests             | [regressions.log](docs/testing/evidence/2026-10-09/regressions.log)                                                                                                                                                |
| Theme audit                        | PASS, 0 failures           | Both light/dark palettes, tokens, contrast, fonts, icons                                                                                                                                                           |
| UI audit, normal scaling           | PASS, 0 failures           | Both themes; 848/1104 px panel widths; keyboard navigation, failed requests, streaming, export integrity, reduced motion                                                                                           |
| UI audit, 200% scaling             | PASS, 0 failures           | [ui-hidpi.log](docs/testing/evidence/2026-10-09/ui-hidpi.log)                                                                                                                                                      |
| Desktop/IPC selftest               | PASS, 0 failures           | Includes four chat failure/recovery scenarios                                                                                                                                                                      |
| Desktop real llama.cpp inference   | PASS                       | Load tiny GGUF → 8 streamed deltas → unload; [log](docs/testing/evidence/2026-10-09/desktop-inference.log)                                                                                                         |
| Real HTTP process smoke            | PASS                       | Startup, route groups, model discovery, llama.cpp completion/chat, dataset CRUD, validation, graceful shutdown; [log](docs/testing/evidence/2026-10-09/http-smoke.log)                                             |
| Real training/adapter/RAG workflow | PASS                       | Tiny GPT-2 inference, 3 training steps, adapter activation, real embeddings/retrieval/chat, export/delete/import, lifespan restart and history recovery; [log](docs/testing/evidence/2026-10-09/real-workflow.log) |
| Additional API boundary matrix     | PASS, no unexpected 500s   | 128 requests across 8 routes; 100 × 422, 15 × 200, 4 × intentional 503, 8 × 400, 1 × 201                                                                                                                           |
| Concurrent dataset storage         | PASS                       | 40 simultaneous creates, 40 reads, 40 deletes; 40 unique IDs, all expected statuses                                                                                                                                |
| Targeted adversarial probes        | 13 findings below          | [probe output](docs/testing/evidence/2026-10-09/probes.log)                                                                                                                                                        |
| Screenshots                        | Captured all 6 panels      | Visually inspected Training, Knowledge, Playground; initial connecting state, not a populated visual acceptance test                                                                                               |
| Relocated AppImage                 | See package addendum below | Existing artifact is an older commit; its results do not establish current-source packaging correctness                                                                                                            |

Run the deterministic probes from the repository root:

```bash
PYTHONPATH=backend/src .venv/bin/python docs/testing/extreme_bug_probe.py
```

[Probe source](docs/testing/extreme_bug_probe.py) uses the actual API handlers and temporary SQLite/filesystem stores. It substitutes small engines/embeddings and controls worker timing where needed; each finding states this. The script prints observations, **not assertions that the buggy behavior should be preserved**. It needs the current backend environment; the PDF finding additionally requires pypdf. No model downloads are needed by the probe itself.

## Findings

### BUG-001 — Deleting a dataset in use destroys the input of an active training job

**High · API/lifecycle · Reproduced with a controlled training worker**

- **Location:** `backend/src/backend/routes/training.py:125`; `backend/src/backend/training/datasets.py` (`delete`); `backend/src/backend/training/jobs.py` (`start`).
- **Reproduce:** Ingest a dataset. Start a job referencing its ID. While the worker is running but before it reads the JSONL file, call `DELETE /datasets/{id}`. Resume the worker.
- **Expected:** Reject with 409 while queued/running jobs reference the dataset, or retain an immutable job-owned copy until the job finishes.
- **Actual:** DELETE returns **204**, removes the JSONL file, and the worker gets **FileNotFoundError**. The probe job ends in `error`.
- **Evidence:** `DELETE_BUSY_DATASET` in the probe log. The substituted worker only creates a deterministic pause before the file read; ingestion, job queue, deletion, and filesystem operations are real.
- **Impact:** A dataset can disappear during model loading or while another job is queued. A valid training submission later fails and the original imported data is gone.
- **Fix / regression:** Coordinate delete and job submission under a shared ownership guard; test both queued and running jobs and the start/delete race.

### BUG-002 — Distinct adapter IDs overwrite the same exported ZIP

**High · Export integrity · Reproduced through API and ZIP files**

- **Location:** `backend/src/backend/routes/adapters.py:74` (`safe = rec.id.replace(":", "-").replace("/", "_")`).
- **Reproduce:** Import adapters with accepted IDs `adapter:collision` and `adapter-collision`, with distinct names/contents. Export the first and retain its download URL; export the second; download the first URL again.
- **Expected:** Distinct adapters have distinct exports, and the first URL still retrieves the first adapter.
- **Actual:** Both return `/adapters/export/adapter-collision.zip`. The second export overwrites the first and the first URL now returns different bytes.
- **Evidence:** `EXPORT_COLLISION`: `first_download_changed: true`. Imports use fixture weights; the ID validation, exports, and HTTP downloads are real.
- **Impact:** A user can save or share the wrong adapter under the first adapter's export result.
- **Fix / regression:** Use an injective ID encoding or per-export UUID and atomic publication. Test colliding punctuation, repeated exports, and simultaneous download/export.

### BUG-003 — UTF-8 BOM in a CSV silently drops instruction outputs

**High · Dataset integrity · Reproduced through API**

- **Location:** `backend/src/backend/training/datasets.py:55`, `:234`.
- **Reproduce:** Ingest CSV content `\ufeffinstruction,output\nQuestion,Answer\n`. The same BOM survives file ingestion because `ingest_path` uses `encoding="utf-8"`.
- **Expected:** Normalize an instruction/assistant pair containing both `Question` and `Answer`.
- **Actual:** Returns **201** and stores only `{"text":"Question"}`. The first header includes the BOM, so instruction-format detection fails and first-column fallback discards the answer.
- **Related manifestation:** BOM-prefixed JSONL `\ufeff{"text":"valid UTF-8 BOM file"}` returns **400**, `Unexpected UTF-8 BOM`.
- **Evidence:** `CSV_BOM`, `JSON_BOM` in probe output.
- **Impact:** Common spreadsheet/editor exports can become incomplete training data without a warning.
- **Fix / regression:** Strip a leading BOM for content ingestion and read files with `utf-8-sig`; compare BOM/no-BOM CSV and JSONL normalization.

### BUG-004 — Missing/non-string sample values are silently trained as Python strings

**Medium · Dataset validation · Reproduced through API and normalized files**

- **Location:** `backend/src/backend/training/datasets.py:184`, `:234`.
- **Reproduce:** Ingest CSV `label,text\ncategory\n`, JSONL `{"content":null}`, or JSONL `{"content":{"bad":"shape"}}`.
- **Expected:** Reject invalid/missing required text with a row-specific 400, consistently with the existing validation for `text` and instruction fields.
- **Actual:** All return **201**. The first two store `{"text":"None"}`; the third stores `{"text":"{'bad': 'shape'}"}`.
- **Evidence:** `CSV_MISSING_TEXT`, `JSON_NULL_CONTENT`, `JSON_OBJECT_CONTENT`.
- **Impact:** Invalid data becomes plausible-looking successful imports and contaminates training. CSV missing values are converted with `str(None)`; JSONL fallback fields similarly bypass type checks.
- **Fix / regression:** Check actual string types and nonempty values before fallback; cover nulls, booleans, numbers, lists, dictionaries, and short CSV rows.

### BUG-005 — A CSV field over 128 KiB causes an unhandled server error

**Medium · Import robustness · Reproduced through API**

- **Location:** `backend/src/backend/training/datasets.py:234`; `backend/src/backend/routes/training.py` (`ingest_dataset`).
- **Reproduce:** Ingest CSV containing `text\n` followed by **131,073** `x` characters. This is one character above this Python runtime's default `csv.field_size_limit()`.
- **Expected:** Accept within a documented import limit, or return a clear 400/413 explaining the limit.
- **Actual:** **500 Internal Server Error**. `csv.Error` escapes the route's `ValueError` handler.
- **Evidence:** `CSV_LONG_FIELD`.
- **Impact:** A long article or code sample prevents the entire dataset from importing, with no actionable explanation.
- **Fix / regression:** Establish an explicit size policy, translate CSV parser errors, and exercise just-below/at/above the limit.

### BUG-006 — A truncated PDF produces an unhandled server error

**Medium · RAG document import · Reproduced with real pypdf extraction**

- **Location:** `backend/src/backend/rag/chunking.py:62`; `backend/src/backend/routes/rag.py:82`.
- **Reproduce:** Create `broken.pdf` with bytes `%PDF-1.7\nthis is truncated`; submit its path to `POST /rag/documents`.
- **Expected:** 400 with a readable corrupt/unsupported PDF explanation and no document registered.
- **Actual:** **500 Internal Server Error**; pypdf emits `EOF marker not found`. The route handles `ValueError`, but the PDF parser's exception is not converted.
- **Evidence:** `BROKEN_PDF`. Embedding availability is forced true to reach extraction; PDF parsing itself is real and happens before embedding.
- **Fix / regression:** Catch PDF read/encryption exceptions at extraction boundaries; test truncated, encrypted, empty, and image-only PDFs separately.

### BUG-007 — Corrupt adapter weights are imported and marked ready

**Medium · Adapter validation · Reproduced through API**

- **Location:** `backend/src/backend/routes/adapters.py:113`.
- **Reproduce:** Import a ZIP with valid `record.json`, `weights/adapter_config.json` containing `{"peft_type":"LORA"}`, and `weights/adapter_model.safetensors` containing the plain text `not a tensor file FIRST`.
- **Expected:** Reject the corrupt tensor file, or explicitly mark the record unvalidated rather than ready.
- **Actual:** **201**, record status **`ready`**. The importer checks that weights are nonempty, not whether they are a valid tensor file.
- **Evidence:** Both `CORRUPT_WEIGHTS_IMPORT` rows. No model loading or tensor execution was used for this reproduction.
- **Impact:** An unusable artifact enters the normal adapter library and fails later at activation.
- **Fix / regression:** Validate safetensors metadata with a safe reader and required PEFT configuration fields. Do not deserialize arbitrary pickle weights just to validate an import.

### BUG-008 — Export reports success when adapter weights are missing

**Medium · Backup/export correctness · Reproduced through API**

- **Location:** `backend/src/backend/routes/adapters.py:74`.
- **Reproduce:** Register/import an adapter, make its registered weights directory unavailable, then call `POST /adapters/export/{id}`. The probe changes the temporary record's path to a nonexistent directory to simulate missing storage.
- **Expected:** Fail clearly instead of producing an unusable backup.
- **Actual:** **200** with a downloadable ZIP containing only `record.json`. The export silently skips weights when `os.path.isdir(rec.path)` is false. Its own importer requires configuration and weights and therefore rejects that archive.
- **Evidence:** `EXPORT_MISSING_WEIGHTS`; export's unconditional `record.json` write and conditional weight traversal.
- **Impact:** A successful export cannot restore the adapter, potentially discovered only when recovery is needed.
- **Fix / regression:** Validate required source files before publishing; ensure failed exports preserve any previous valid artifact.

### BUG-009 — Raw HTML ingestion indexes scripts/markup instead of extracted text

**Medium · Retrieval correctness · Reproduced through API with deterministic embeddings**

- **Location:** `backend/src/backend/routes/rag.py:82`.
- **Reproduce:** Ingest `<script>secret irrelevant javascript</script><p>Actual answer</p>` once as raw content with `kind: "html"`, and once through an `.html` file path. Query the store.
- **Expected:** Both forms index equivalent extracted visible text.
- **Actual:** Both return **201**, but raw-content retrieval returns the entire HTML including script contents; file-based retrieval returns `Actual answer`.
- **Evidence:** `RAW_HTML`. Only embedding generation is substituted; ingestion, extraction, stored chunks, and retrieval are real.
- **Impact:** The input method changes retrieval semantics and can place irrelevant scripts/styles into model context. Raw Markdown similarly skips the extraction branch.
- **Fix / regression:** Share content extraction between path/raw inputs and define valid raw PDF semantics; compare stored chunks across input methods.

### BUG-010 — Inference accepts a requested model but silently uses another

**Medium · API contract · Reproduced through API with a fake engine**

- **Location:** `backend/src/backend/inference/schemas.py` (`ChatRequest`, `CompletionRequest`); `backend/src/backend/routes/inference.py:202`, `:211`.
- **Reproduce:** With model A loaded, send chat with `model: "different-B"` and `stream: false`.
- **Expected:** Either reject a mismatch or deliberately resolve/load the requested model. A silently ignored selection should not be an accepted request field.
- **Actual:** **200**, generated with A; response `model` is `loaded-A`. Both routes ignore their schema's `model` field.
- **Evidence:** `MODEL_IGNORED`. Fake generation isolates routing; this is not a claim that a real model B was loaded.
- **Impact:** API consumers can associate experiments/results with a requested model that did not run. The desktop currently relies on the loaded engine rather than this field.
- **Fix / regression:** Validate requested model identity against the active canonical model, or remove/deprecate the field explicitly. Cover chat and completion.

### BUG-011 — Training and adapter activation ignore registered model paths/revisions

**Medium · Model resolution · Reproduced at the resolver boundary**

- **Location:** `backend/src/backend/training/trainer.py:52`; `backend/src/backend/inference/supervisor.py` (`_activate_adapter`).
- **Reproduce:** Register `huggingface:owner--model` with a local `path` pointing at a particular snapshot; call `_resolve_base` for that registered ID.
- **Expected:** Use the registered local path/revision, as ordinary inference resolution does.
- **Actual:** Returns **`owner/model`**, disregarding the record. Training and adapter activation use this helper before their `from_pretrained` calls.
- **Evidence:** `TRAINING_RESOLUTION` logs the different registered and resolved values. No remote download or real alternate revision was used in this targeted probe.
- **Impact:** Local/custom snapshots can fail offline or cause network resolution; a non-default registered revision can diverge from the base used by training or adapter activation. The normal cached tiny-GPT-2 workflow passing does not cover this case.
- **Fix / regression:** Share registry-aware resolution, preserve revision/path identity in adapter metadata, and test offline snapshots outside the default cache.

### BUG-012 — Non-streaming inference stops monitoring disconnects after the first token

**Medium · Cancellation/resource lifecycle · Reproduced with controlled request/generator**

- **Location:** `backend/src/backend/routes/inference.py:161` (`_response`).
- **Reproduce:** Invoke `_response` with `stream=False`, a generator yielding immediately then pausing 250 ms, and a request that becomes disconnected after 50 ms.
- **Expected:** Observe disconnect during generation and close the generator promptly, releasing the inference operation.
- **Actual:** Disconnect is checked only **once**, during `_first_token`; the response consumes the rest and includes `after disconnect` even though the request is now disconnected.
- **Evidence:** `NONSTREAM_DISCONNECT_WATCH`: `disconnect_checks: 1`, `disconnected: true`.
- **Impact:** Work can continue after a non-streaming caller disappears, retaining the single inference reservation until completion. The desktop streaming route has separate protections and is not implicated by this probe.
- **Fix / regression:** Keep disconnect monitoring throughout non-streaming consumption and ensure `aclose()`/worker cancellation on exit. Follow up with an actual dropped HTTP connection test; this reproduction is at the handler boundary.

### BUG-013 — Cancellation during adapter saving is ignored and publishes a completed adapter

**Medium · Training cancellation · Reproduced using the actual trainer control flow**

- **Location:** `backend/src/backend/training/trainer.py:319`, `:322`, `:362`.
- **Reproduce:** Run `run_training` with lightweight ML doubles; set the actual cancellation event inside `model.save_pretrained`, after the pre-save cancellation check.
- **Expected:** Honor accepted cancellation before publishing the adapter, or make finalization explicitly non-cancellable and reject cancellation requests during that stage.
- **Actual:** Event is set, but the job becomes **`done`** and an adapter is registered. There is no cancellation check between saving and publication; the manager still accepts cancellation while job status is `running`.
- **Evidence:** `CANCEL_DURING_SAVE`: `cancelled: true`, `status: "done"`, `adapter_registered: true`.
- **Impact:** Stop can appear accepted while a slow save is underway, yet the operation finishes and publishes its result anyway.
- **Fix / regression:** Define a synchronized finalization boundary; check/commit cancellation and publication atomically and clean up any cancelled output.

## Other observations and limits

- Empty chat message lists are accepted by schema and returned 200 with the fake engine. This is a validation gap to assess against the intended API contract, **not counted as a separate confirmed real-engine bug**.
- Escaped unpaired Unicode in dataset content returned 400. The HTTP test client initially rejected this input before sending it; the probe was corrected to send escaped JSON, so the client-side error is not reported as an app bug.
- Initial sandbox runs blocked loopback binding and stalled async tests. Unrestricted reruns passed; these environment failures are not counted as defects. Stalled test processes were stopped.
- Tiny GGUF was initially missing from cache. After the authorized desktop download, real HTTP inference passed.
- Real-model testing establishes pipeline execution, not training quality, numerical stability, or answer accuracy. A real training log includes a first-step `grad_norm: nan`; training completed, and numerical quality was not investigated sufficiently to classify this as a defect.
- No full disk exhaustion, deliberate OOM, power-loss SQLite corruption, multi-hour soak, large-model VRAM stress, live Ollama/vLLM serving, Windows/macOS behavior, or native display-server/accessibility-reader validation was performed. Offscreen UI tests do not replace those checks.
- Existing archive traversal, duplicate import, shared-cache deletion, busy inference, disconnected loading, and stream cleanup regressions passed. The probes do not establish a general security certification.

## Recommended repair order

1. Prevent dataset deletion during training, eliminate export filename collisions, and preserve BOM-prefixed training data (BUG-001–003).
2. Repair sample/import validation and export integrity (BUG-004–008).
3. Make model identity, cancellation, and retrieval behavior consistent (BUG-009–013).
4. Turn each reproduction into an assertion for the intended fixed behavior and rerun the existing 41 regressions plus real tiny-model workflows.

## Packaged-app addendum

The historical pre-preview AppImage (superseded; not the current preview artifact) passed **all seven** relocated checks: extraction, theme, UI, desktop/backend selftest, real GGUF inference, combined LoRA/adapter/RAG workflow, and the actual AppImage extract-and-run launcher. The test supplied invalid host `PYTHONHOME`/`PYTHONPATH` and a restricted `PATH` to verify runtime isolation.

```bash
python3 packaging/smoke_appimage.py dist/Magnite-v0.1-preview-linux-x86_64.AppImage \
  --logs /tmp/magnite-extreme-audit/package \
  --cache /tmp/magnite-extreme-audit/package-cache --features
```

[Package summary](docs/testing/evidence/2026-10-09/package-summary.log). `dist/verification/build.json` identifies the packaged commit as **6c83de5b2bded93bd52f49d75fd7752b1fa7f6c6**, distinct from the audited source. This artifact was tested as supplied, not rebuilt. Full temporary logs and six panel screenshots are under `/tmp/magnite-extreme-audit/`; durable selected evidence is under `docs/testing/evidence/2026-10-09/`.

## Repair verification — 2026-10-09

BUG-001 through BUG-013 are addressed in the current working tree. The original
findings above describe the audited source before these changes.

- Dataset ownership guards coordinate submission/deletion; BOM normalization,
  strict value checks, and explicit CSV limits prevent lossy imports.
- Exports use unique URLs and atomic publication, reject missing weights, and
  imports validate LoRA configuration and safetensors without loading pickle.
- PDF failures return input errors; raw HTML/Markdown shares file extraction.
- Inference validates requested model identity and monitors disconnects for the
  full response. Training/activation preserves registered snapshot paths.
- Training cancellation and publication share a lock; cancellation during save
  removes the output and does not register an adapter.

Verification: **53 offline backend tests passed**, including the real dropped
HTTP connection regression. The real cached tiny-model workflow passed training,
adapter activation, RAG, export/delete/import, and restart recovery. The real
llama.cpp process/HTTP smoke passed inference, dataset CRUD, and graceful shutdown.
The existing AppImage was not rebuilt as part of these backend fixes.

Regression assertions live in `backend/tests/test_october_fixes.py`, with the
save-cancellation case in `backend/tests/test_backend_audit.py`. The observation
probe now supplies valid adapters for export checks after testing corrupt import
rejection, and handles cancelled non-streaming responses.

Repair evidence: [regressions](docs/testing/evidence/2026-10-09-fixes/regressions.log), [real workflow](docs/testing/evidence/2026-10-09-fixes/real-workflow.log), [HTTP smoke](docs/testing/evidence/2026-10-09-fixes/http-smoke.log), and [updated probes](docs/testing/evidence/2026-10-09-fixes/probes.log).
