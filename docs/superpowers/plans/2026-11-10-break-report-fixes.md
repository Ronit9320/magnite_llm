# Break-Report Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix all 13 findings in `docs/testing/BREAK_REPORT_2026-11-10.md`, and after each fix re-run the exact break that exposed it to prove it now holds.

**Architecture:** One re-runnable probe script (`docs/testing/break_recheck.py`) reproduces every finding against a standalone backend in a scratch `MAGNITE_HOME`. Each task runs its probe first (must print `BROKEN`), adds a unit test (red), fixes, then runs the unit test (green) and the same probe again (must print `HELD`). Backend fixes are Python; #2, #12, #13 also touch the C++ client.

**Tech Stack:** Python 3.13 / FastAPI / SQLite / llama-cpp (backend); C++20 / Qt6 (client); `unittest`; `httpx.ASGITransport` test fixtures from `backend/tests/test_regressions.py`.

**Spec:** `docs/testing/BREAK_REPORT_2026-11-10.md` (the findings are numbered `#1`–`#13`; tasks below use the same numbers).

## Global Constraints

- **Branch `fix`, local only: never push, open a PR, or dispatch a workflow without asking the user first** (workflows trigger only on `dev`/`master` pushes, PRs into them, or manual dispatch, so `fix` itself runs no Actions; a PR would). **Do not commit.** Memory rule `no-git-commits-macos-release`: all changes stay local and uncommitted. No task has a commit step. If a commit is ever requested, add no Claude attribution (memory rule `no-claude-attribution`).
- **Branch:** `fix`, cut from `feature`, which is the same commit as `origin/dev` (`1c5e506`), so it satisfies the `AGENTS.md` rule.
- Offline regression command (the "full suite"): `PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests -v`. It must stay green after every task.
- New tests go in `backend/tests/test_break_fixes_2026_11.py` (one `TestCase` class per finding, named `Finding<N>...`), reusing `test_regressions.ApiTests` fixtures. `backend/tests` has no `__init__.py`, so run them with discovery, never a file path. "Targeted run" below means: `PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests -p test_break_fixes_2026_11.py -k Finding<N> -v`.
- Client checks: `./build/magnite-app --ui-check` (existing UI suite, covers `ChatStore`), `--theme-check` (existing), `--restart-policy-check` (new, break-02 plan Task A1).
- **Manual GUI runs must set `MAGNITE_HOME=<scratch dir>`** (the app otherwise uses `~/.magnite`). **A scratch `HOME` does NOT isolate the app's Qt preferences on macOS** (found while executing: Qt reads `~/Library/Preferences/com.magnite-local.Magnite LLM.plist` through the system preferences service, ignoring `$HOME`); only `--ui-check` and `--selftest` redirect them to a temp folder. GUI runs therefore read the user's real saved preferences (sidebar state, name); never change them for a test. They must only kill the backend that *that* app instance spawned (`pgrep -P <app pid>`), never `pkill -f backend.main`, which could hit a real running Magnite.
- New C++ files must be added to `MAGNITE_SOURCES` in `app/CMakeLists.txt`. Build: `cmake --build build -j`. No hardcoded hex colors (`Theme` tokens only).
- Probes never touch `~/.magnite`: scratch `MAGNITE_HOME` only. Inference probes need the cached SmolLM2-135M-Instruct Q4_K_M GGUF, passed as `--gguf <path>`.
- Never change what held up under attack (report "Held up" section): auth/Host checks, chat edge cases, cancel timing, NaN-loss filtering. Task 13 re-runs them.

## Review Focus

Failure modes the report implies but no finding spells out, most likely first:

1. **Corrupt DB with leftover `-wal`/`-shm` files**: quarantine must move the sidecars too, or the fresh DB inherits a stale journal. Pinned in Task 1.
2. **Quarantine must not destroy data**: a bad job row is kept (in a quarantine table), not deleted, so a version mismatch is recoverable. Pinned in Task 1.
3. **Auto-restart wires signals twice**: `MainWindow::onBackendStarted` calls `connect(...)` unconditionally, so every restart would double every panel signal. Pinned in the break-02 plan, Task A1.
4. **HTML text must stay the same for ordinary pages**: a linear parser must still drop `<script>`/`<style>` bodies, unescape entities, and keep words separated. Pinned in Task 3.
5. **Failed swap with no way back**: if `engine.load()` fails after the old model is unloaded, the user is left with nothing. Rollback to the previous model is pinned in Task 4.

---

## File Structure

| File | Responsibility |
|---|---|
| `docs/testing/break_recheck.py` (new) | One probe per finding; prints `#N HELD\|BROKEN\|MANUAL <evidence>`; `--only 1,3`, `--gguf PATH` |
| `backend/tests/test_break_fixes_2026_11.py` (new) | Unit/API regression tests, one class per finding |
| `backend/src/backend/database.py` | + `open_or_quarantine(path, init)`; quarantine notices registry |
| `backend/src/backend/request_limits.py` (new) | ASGI middleware: reject oversized request bodies with 413 |
| `app/src/restart_policy.h` (new, header-only; see break-02 plan) | Backoff/attempt-budget logic for backend restart, unit-testable |

Where the plan departs from the report's fix direction, and why: #5 also covers `base_model` (also attacker-controlled), #7 prunes by age because the existing test `test_export_urls_are_unique_and_immutable` forbids reusing one file, #12's fix has to fire at signal time because uvicorn drains open streams *before* it runs the shutdown hook.

---

### Task 0: Probe harness (baseline: every backend finding reproduces)

**Files:**
- Create: `docs/testing/break_recheck.py`

**Interfaces:**
- Produces: `probe_<N>(ctx: Ctx) -> Result` for N in 1,2,3,4,5,6,7,8,9,10,11,12 plus `probe_2b`; `Result(status: Literal["HELD", "BROKEN", "MANUAL", "SKIP"], evidence: str)`; `Ctx.start_backend(home: str | None = None, env: dict | None = None) -> Backend` (`Backend.url`, `.proc`, `.stop()`, `.get/.post(path, **kw)`); `Ctx.gguf: str | None`. Probe 13 returns `MANUAL` with the steps from Task 12. Probe 2 is the backend half of the 2026-11-10 reproduction (below); its GUI half is the manual run in break-02 Task A1. `probe_2b` is the Phase B check and returns `SKIP (Phase B not started)` until `backend.training.train_worker` is importable.

- [ ] **Step 1: Write the harness.** `main()` parses `--only` and `--gguf`, makes a scratch home per probe (`tempfile.mkdtemp`), launches `.venv/bin/python -m backend.main --port 0 --home <scratch>` (parse the port from the stderr line `Magnite backend starting on http://127.0.0.1:<port>`), and exits with the count of `BROKEN` results among those selected. Probes skip with `SKIP (needs --gguf)` when they need a model and none was given.
- [ ] **Step 2: Port each repro verbatim from the report** (the report's "Reproduce" lines are the spec). Pass criteria ("HELD") per probe:
  - #1 backend still starts and `/health` is 200 after seeding `('x','{not json')` into `training_jobs.db`, and separately after writing `this is not sqlite` into `models.db`.
  - #2 (backend half; needs `--gguf` and the cached `HuggingFaceTB/SmolLM2-135M`): load the GGUF, ingest a 60-line text dataset, `POST /training/start` (`epochs=3, max_steps=400, batch_size=1, max_length=64`), wait for `current_step >= 3`, `kill -9` the backend, relaunch on the same home. HELD when the job comes back `error` with `current_step >= 3`; baseline is `step 0/0`. `probe_2b` (Phase B): same setup, then `kill -9` the *worker* instead; HELD when chat, `/models` and `/rag/documents` still answer 200 and the job is `error` with the "ran out of memory" message.
  - #3 `POST /rag/documents {kind:"html", content:"a < b "*60000}` finishes in < 2 s and max `/health` latency while it runs is < 1 s.
  - #4 after a failed `/inference/load` of `/nonexistent/x.gguf`, `/inference/status.loaded_model` is unchanged.
  - #5 `/inference/adapter` for an imported zip naming `attacker-org/some-model` returns 400 and makes no Hub request.
  - #6 `max_steps=10**12` and `seed=2**40` both return 4xx from `POST /training/start` before any job is queued (needs an ingested dataset first: `POST /datasets/ingest`).
  - #7 three exports of one adapter then a prune-eligible old file (set with `os.utime` to 2 days ago) leaves only the fresh files.
  - Probes #5 and #7 need an importable adapter zip: build it with the same recipe as `test_regressions.ApiTests.archive` (a tiny valid `adapter_model.safetensors` plus `adapter_config.json` with `peft_type`).
  - #8 a 60 MB `/datasets/ingest` body gets 413; a 10 MB `/inference/context-usage` body returns 200 in < 1 s with `exact == false` (it was a 4.3 s tokenization).
  - #9 the 4-thread/15-cycle race produces zero 500s and zero resets.
  - #10 after a `\ud800` dataset ingest (400) `datasets/` has no new file; the RAG variant returns 400 with a message containing "invalid character".
  - #11 `POST /inference/completion {"prompt": ""}` returns 400 with a non-empty `detail`.
  - #12 SIGTERM during an active stream exits in < 2 s.
- [ ] **Step 3: Baseline run.** Run: `.venv/bin/python docs/testing/break_recheck.py --gguf <path>`. Expected: every automated probe prints `BROKEN` (this proves each break reproduces before fixing; if any prints `HELD`, stop and re-read the report for that finding before touching code). Also run the offline suite once and write the passing test count into this plan's progress notes; Task 13 compares against it.

---

### Task 1: #1 Tolerant startup

**Files:**
- Modify: `backend/src/backend/database.py`, `backend/src/backend/training/jobs.py:36-48` (inline in `__init__`, no `_init_db`), and the `_init_db` of `models/store.py`, `adapters/store.py`, `rag/store.py`, `training/datasets.py`
- Modify: `backend/src/backend/routes/health.py` (`HealthResponse` gains `notices: list[str] = []`; the `/health` route is unauthenticated, so notices carry file names only, never full paths)
- Modify: `app/src/mainwindow.cpp` (`onBackendStarted`: show notices once)
- Test: `backend/tests/test_break_fixes_2026_11.py::Finding1TolerantStartup`

**Interfaces:**
- Produces: `database.open_or_quarantine(path: str, init: Callable[[sqlite3.Connection], None]) -> None` (runs `init` inside `connection(path)`; on `sqlite3.DatabaseError` moves `path`, `path-wal`, `path-shm` to `<path>.corrupt-<UTC YYYYmmddTHHMMSS>`, appends a human-readable string to `database.NOTICES: list[str]`, retries `init` once on a fresh file); `/health` JSON gains `notices: list[str]`.

- [ ] **Step 1: Write failing tests.**
  - `test_garbage_models_db_is_quarantined`: write `b"this is not sqlite"` to `<home>/models.db` plus empty `models.db-wal`; construct `ModelStore`; assert it works (`list()` is `[]`), `models.db.corrupt-*` exists with the original bytes, no `-wal` remains beside the new DB, and `database.NOTICES` has one entry naming `models.db`.
  - `test_bad_job_row_is_quarantined_not_dropped`: seed `jobs` with one valid job and `('x','{not json')`; construct `TrainingJobManager`; assert `list()` has only the valid job and the bad row is still readable from table `jobs_quarantine(id, record, error)`.
  - `test_health_reports_notices`: after a quarantine, `GET /health` returns `notices` with the entry.
- [ ] **Step 2: Targeted run** (`Finding1`). Expected: FAIL (`DatabaseError` / `ValidationError`).
- [ ] **Step 3: Implement** `open_or_quarantine` and call it from the four stores' `_init_db` and from the job manager's `__init__`. In `jobs.py`, wrap `model_validate_json` per row; on `ValidationError`, `INSERT` into `jobs_quarantine`, `DELETE` from `jobs`, append a notice, continue. The notice for a quarantined `models.db` should say registered models must be re-added (the files on disk are untouched).
- [ ] **Step 4: Surface to the user (C++).** After `/health` succeeds in `onBackendStarted`, if `notices` is non-empty show one `QMessageBox::information` ("Magnite repaired its data files: ...") using the same `BackendClient` GET pattern used elsewhere in `mainwindow.cpp`.
- [ ] **Step 5: Targeted run** (`Finding1`). Expected: PASS. Then the full suite: PASS.
- [ ] **Step 6: Re-break.** `.venv/bin/python docs/testing/break_recheck.py --only 1`. Expected: `#1 HELD`.

---

### Task 2: #2 Backend crash recovery: moved to its own plan

**Moved to `docs/superpowers/plans/2026-11-10-break-02-backend-recovery.md`.** That plan holds Phase A (auto-restart with backoff in Task A1, and saving training progress while a job runs in Task A2) and Phase B (training in a child process, gated behind an explicit go from the user after Task 13). Execute Phase A at this point in the sequence, right after Task 1: restart must not run before tolerant startup, or a corrupt database burns the restart budget. Its results go in this plan's progress notes and in Task 13.

---

### Task 3: #3 Linear-time HTML extraction

**Files:**
- Modify: `backend/src/backend/rag/chunking.py:44-52` (`_HANDLER_TAG`, `_TAG`, `_strip_html`)
- Test: `Finding3LinearHtml`

**Interfaces:**
- Produces: `_strip_html(text: str) -> str` unchanged signature, now backed by `class _TextExtractor(html.parser.HTMLParser)` (`convert_charrefs=True`; collects text data, emits one space for every tag boundary, ignores data inside `script`/`style`).

- [ ] **Step 1: Write failing tests.**
  - `test_stray_lt_is_linear`: `extract_content("a < b " * 80000, "html")` returns in < 1.0 s.
  - `test_unclosed_scripts_are_linear`: `"<script>x" * 20000` returns in < 1.0 s.
  - `test_ordinary_page_unchanged` (Review Focus 4): `"<p>Hello&nbsp;<b>world</b></p><script>var a=1;</script><style>p{}</style><p>x &amp; y &#x1F600;</p>"` yields tokens `["Hello", "world", "x", "&", "y", "😀"]` after `.split()`, with no `var`/`p{}` present, and `"a<b>c</b>d".split()` is `["a","c","d"]`.
- [ ] **Step 2: Targeted run** (`Finding3`). Expected: linear tests FAIL (timeout/slow); ordinary-page test passes today (it is the behavior to preserve).
- [ ] **Step 3: Implement** the parser-based `_strip_html`; delete the two regexes.
- [ ] **Step 4: Targeted run** (`Finding3`), then the full suite (includes the existing HTML check at `test_october_fixes.py:98`). Expected: PASS.
- [ ] **Step 5: Re-break.** `break_recheck.py --only 3`. Expected: `#3 HELD` (ingest < 2 s, `/health` max latency < 1 s).

---

### Task 4: #4 A failed load must not unload the working model

**Files:**
- Modify: `backend/src/backend/inference/supervisor.py` (`_load_model` lines 168-190, `_activate_adapter` lines 192-228)
- Test: `Finding4LoadKeepsModel`

**Interfaces:**
- Produces: `InferenceSupervisor._restore(previous: tuple[str, str, AdapterRecord | None] | None) -> None` (best-effort reload of `(engine_name, model_ref, active_adapter)`; adapter engines are rebuilt with `TorchPeftEngine(base, rec.path)`; never raises, logs on failure). Behavior: `_load_model` first validates the new reference cheaply, **only when it is an absolute path** (registry/ollama refs skip this): it must exist; a file for the `llamacpp` engine must be non-empty and start with the GGUF magic `GGUF`; a directory is accepted as is. A validation failure raises `ValueError` **before** `_unload()`. If `engine.load()` fails after unload, `_restore(previous)` reloads the previous model, then the original exception propagates. Capture `previous` before `_unload()`.

- [ ] **Step 1: Write failing tests** (supervisor with a `FakeEngine` and patched `make_engine`):
  - `test_missing_path_keeps_old_model`: load `A`, load `/nonexistent/x.gguf` → raises `ValueError`; `status().loaded_model == A`; `_engine` is the same object.
  - `test_load_failure_restores_previous` (Review Focus 5): `make_engine` returns an engine whose `load()` raises for `B`; after the failed load of `B`, `status().loaded_model == A` and the active engine loads `A` again.
  - `test_adapter_load_failure_restores_previous`: same for `activate_adapter`.
  - `test_failed_load_with_nothing_loaded_stays_empty`: no previous model → no restore attempted, no error masking.
- [ ] **Step 2: Targeted run** (`Finding4`). Expected: FAIL.
- [ ] **Step 3: Implement** validation-before-unload and `_restore`; capture `previous = (self._engine.name, self.model_ref, self.active_adapter)` before `_unload()`. Add one more test: failed load while an adapter is active restores the adapter engine.
- [ ] **Step 4: Targeted run** (`Finding4`) and the full suite. Expected: PASS.
- [ ] **Step 5: Re-break.** `break_recheck.py --only 4 --gguf <path>`: load the SmolLM2 GGUF, request `/nonexistent/x.gguf`, expect 400 and `/inference/status.loaded_model` still the GGUF. Expected: `#4 HELD`.

---

### Task 5: #5 Imported adapter cannot choose a Hub model

**Files:**
- Modify: `backend/src/backend/routes/adapters.py` (`_import_adapter`, after `model_validate_json` at line ~125), `backend/src/backend/inference/supervisor.py:206` (`_activate_adapter`), `backend/src/backend/models/resolution.py`
- Test: `Finding5ImportedBase`

**Interfaces:**
- Produces: `resolution.resolve_base_offline(ref: str) -> str` in `models/resolution.py`: calls the existing `resolve_base(ref)`; if the result is an existing directory it returns it, otherwise it treats it as a Hub repo id and returns `huggingface_hub.snapshot_download(ref, local_files_only=True)`, converting `LocalEntryNotFoundError`/`OSError`/`ValueError` into `ValueError("Adapter base model is not downloaded: <ref>")`. It never touches the network. `_activate_adapter` calls it in place of `rec.meta.get("resolved_base_model") or _resolve_base(rec.base_model)` and never reads `meta["resolved_base_model"]`.

- [ ] **Step 1: Confirm the gap.** `resolution.resolve_base` returns a bare repo id unchanged (only `huggingface:` refs go through the registry), and the torch engine then calls `from_pretrained`, which is the Hub fetch the report saw. The report names only `meta`, but `rec.base_model` also comes from the zip's `record.json`, so the offline resolver must cover both.
- [ ] **Step 2: Write failing tests.**
  - `test_import_strips_resolved_base_model`: after importing a zip whose meta has `resolved_base_model`, the stored record's `meta` has no such key.
  - `test_activate_with_unknown_base_is_400_no_network`: import a zip with `base_model="attacker-org/some-model"`; `POST /inference/adapter` returns 400, and a patched `huggingface_hub`/`from_pretrained` raises if called (it must not be).
  - `test_activate_locally_trained_adapter_still_works`: an adapter whose base is a cached/local dir activates as before (guards against over-blocking).
- [ ] **Step 3: Targeted run** (`Finding5`). Expected: first two FAIL.
- [ ] **Step 4: Implement** (drop `resolved_base_model` from `rec.meta` at import; `resolve_base_offline` at activation). The `/inference/adapter` route already maps `ValueError` to 400, so no route change is needed.
- [ ] **Step 5: Targeted run** (`Finding5`) and full suite. Expected: PASS.
- [ ] **Step 6: Re-break.** `break_recheck.py --only 5` (builds the malicious zip, imports, activates). Expected: `#5 HELD`.

---

### Task 6: #6 Training limits

**Files:**
- Modify: `backend/src/backend/training/schemas.py:35-41`, `backend/src/backend/training/jobs.py` (`_validate`, line 94)
- Test: `Finding6TrainingLimits`

**Interfaces:**
- Produces: `MAX_TOTAL_STEPS = 100_000` in `schemas.py`; `TrainingConfig.max_steps: int = Field(0, ge=0, le=MAX_TOTAL_STEPS)`; `TrainingConfig.seed: int = Field(42, ge=0, le=2**32 - 1)`; `_validate` raises `ValueError(f"Training would run {n} steps; the limit is {MAX_TOTAL_STEPS}")` when `max_steps == 0` and `epochs * ceil(rows / batch_size) > MAX_TOTAL_STEPS`.

- [ ] **Step 1: Write failing tests:** `max_steps=10**12` → `ValidationError`; `seed=2**40` → `ValidationError`; `seed=-1` → `ValidationError`; a dataset record patched to `rows=1_000_000` (no real file needed), `epochs=100`, `batch_size=1` → `TrainingJobManager.start` raises `ValueError` mentioning the limit; `max_steps=100_000` and `seed=2**32-1` are accepted.
- [ ] **Step 2: Targeted run** (`Finding6`). Expected: FAIL.
- [ ] **Step 3: Implement** the schema bounds and the derived-steps check in `_validate`.
- [ ] **Step 4: Targeted run** (`Finding6`) and full suite. Expected: PASS.
- [ ] **Step 5: Re-break.** `break_recheck.py --only 6`. Expected: `#6 HELD` (both submits return 4xx immediately, no model load).

---

### Task 7: #7 Export cleanup

**Files:**
- Modify: `backend/src/backend/routes/adapters.py` (`export_adapter`, line 79)
- Test: `Finding7ExportPrune`

**Interfaces:**
- Produces: `_prune_exports(exports: str, max_age_s: float = 86400.0, now: float | None = None) -> int` (deletes `*.zip` older than `max_age_s` and `.export-*` temp files older than 3600 s; returns the count removed; ignores files that vanish or cannot be removed, e.g. open on Windows). `export_adapter` calls it before writing the new zip. Exports stay unique and immutable within the window, because the existing test `test_export_urls_are_unique_and_immutable` requires that; reuse-one-file-per-adapter is therefore rejected.

- [ ] **Step 1: Write failing test:** create `old.zip` and `.export-stale` with `os.utime` set 2 days back, and `fresh.zip` now; call `_prune_exports`; old files gone, fresh kept, returns 2. Then an API test: export twice, make the first file old, export again, first file is gone.
- [ ] **Step 2: Targeted run** (`Finding7`). Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Targeted run** (`Finding7`) and the full suite (includes `test_october_fixes`). Expected: PASS.
- [ ] **Step 5: Re-break.** `break_recheck.py --only 7`. Expected: `#7 HELD`.

---

### Task 8: #8 Request size limits

**Files:**
- Create: `backend/src/backend/request_limits.py`
- Modify: `backend/src/backend/server.py` (register middleware), `backend/src/backend/training/schemas.py` (`DatasetIngestRequest.content`), `backend/src/backend/rag/schemas.py` (`RagIngestRequest.content`), `backend/src/backend/inference/schemas.py` (`ContextUsageRequest`)
- Test: `Finding8RequestLimits`

**Interfaces:**
- Produces: `class BodyLimitMiddleware` (ASGI; `__init__(app, max_bytes: int = 32 * 1024**2)`); rejects with `413 {"detail": "Request body too large (limit 32 MiB)"}` using `Content-Length` when present and a running byte count for chunked bodies. `DatasetIngestRequest.content` and `RagIngestRequest.content` get `max_length=16_000_000` (a body between 16M characters and 32 MiB therefore gets FastAPI's 422, not 413; both are acceptable "rejected" outcomes). The chat schemas have no character limit (a 2 MB chat message is rejected only because it exceeds the context, after tokenizing), so the meter needs its own cap: `MAX_METER_CHARS = 1_000_000` in `routes/inference.py`; above it `context_usage` skips tokenizing and returns `ContextUsage(used=total_chars // 3, reserved=req.max_tokens, limit=engine.context_size(), fill=<saturated via fill_of>, exact=False)`, so the meter shows "full" instead of an error or a 4 s stall.

- [ ] **Step 1: Write failing tests:** a body of 33 MiB to `/datasets/ingest` → 413 and the handler never ran (patch `store.ingest_content` and assert not called); a body under both limits is accepted; a chunked body without `Content-Length` over the cap → 413; a `/datasets/ingest` `content` of 17M characters → 422; `context-usage` with a 10 MB message returns 200 in < 1 s with `fill >= 1.0`, `exact == False`, and the engine's `count_messages` not called.
- [ ] **Step 2: Targeted run** (`Finding8`). Expected: FAIL.
- [ ] **Step 3: Implement** the middleware (register in `server.py` beside `UpdateGate`) and the field limits; keep `/adapters/import` and every path-based endpoint unaffected (they send small JSON).
- [ ] **Step 4: Targeted run** (`Finding8`) and full suite. Expected: PASS.
- [ ] **Step 5: Re-break.** `break_recheck.py --only 8 --gguf <path>`. Expected: `#8 HELD`.

---

### Task 9: #9 Context meter vs unload race

**Files:**
- Modify: `backend/src/backend/inference/engines/llamacpp.py:142-150` (and the torch/vllm/ollama engines' `count_tokens`/`count_messages` if they dereference unloadable state), `backend/src/backend/routes/inference.py:64-75`
- Test: `Finding9MeterRace`

**Interfaces:**
- Produces: `class EngineUnloaded(RuntimeError)` in `engines/base.py`; `LlamaCppEngine` holds `self._state_lock = threading.Lock()`; `count_tokens` takes it and raises `EngineUnloaded` when `_llm is None`; `unload()` takes the same lock before nulling `_llm` (so a tokenize in flight finishes before the native context is freed). The route catches `EngineUnloaded` and returns 409.

- [ ] **Step 1: Write failing tests:** (a) engine unit test: `count_tokens` after `unload()` raises `EngineUnloaded`, not `AttributeError`; (b) API test with a fake engine whose `count_messages` raises `EngineUnloaded` returns 409; (c) threaded test: 4 threads call `/inference/context-usage` while another alternates unload/load 15 cycles; assert every status is in `{200, 409}`.
- [ ] **Step 2: Targeted run** (`Finding9`). Expected: FAIL.
- [ ] **Step 3: Implement.** Apply the same guard to `TorchPeftEngine.count_tokens`/`count_messages` (`engines/torch.py:134-137`) if they dereference state that `unload()` clears.
- [ ] **Step 4: Targeted run** (`Finding9`) and full suite. Expected: PASS.
- [ ] **Step 5: Re-break with the real model.** `break_recheck.py --only 9 --gguf <path>`. Expected: `#9 HELD` with zero 500s **and zero connection resets**. If resets persist while 500s are gone, they are a separate cause: stop and use superpowers:systematic-debugging before claiming #9 fixed.

---

### Task 10: #10 and #11 Bad input messages

**Files:**
- Modify: `backend/src/backend/training/datasets.py:88-92` (`ingest_content`), `backend/src/backend/routes/rag.py` (ingest handler), `backend/src/backend/routes/inference.py:307` (`completion`)
- Test: `Finding10BadCharacters`, `Finding11EmptyPrompt`

**Interfaces:**
- Produces: `ingest_content` builds the full JSONL text and calls `.encode("utf-8")` **before** creating any file, writes to a temp file in the same directory and `os.replace`s it, and removes the temp file on any failure; the error message is `"Dataset contains a character that cannot be saved (lone surrogate at position N)"` (ValueError → 400). RAG ingest validates the extracted text with `.encode("utf-8")` before chunking/embedding and returns 400 `"Document contains an invalid character (lone surrogate at position N)"`. `completion` returns 400 `{"detail": "prompt must not be empty"}` when `req.prompt.strip()` is empty (whitespace-only counts as empty; an intentional small extension of the report's "empty").

- [ ] **Step 1: Write failing tests:** dataset ingest of `"a\ud800b"` → 400 and the dataset directory contains no `.jsonl` or temp files afterwards; RAG ingest of the same → 400 with `"invalid character"` in `detail` (not 503); completion with `""` and with `"   "` → 400 with non-empty `detail`.
- [ ] **Step 2: Targeted run** (`-k "Finding10 or Finding11"`). Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Targeted run** (same `-k`) and full suite. Expected: PASS.
- [ ] **Step 5: Re-break.** `break_recheck.py --only 10,11 --gguf <path>`. Expected: `#10 HELD`, `#11 HELD`.

---

### Task 11: #12 Quit during generation

**Files:**
- Modify: `backend/src/backend/inference/engines/streaming.py` (`threaded_stream`), `backend/src/backend/main.py` (server construction at line ~83 and `request_shutdown`), `app/src/mainwindow.cpp` (`~MainWindow`, line 147) only if Step 6 shows a visible freeze
- Test: `Finding12FastShutdown`

**Why the hook in `server.py` is the wrong place:** uvicorn's SIGTERM handler sets `should_exit`, then waits for open connections (including streaming responses) to finish *before* it runs the lifespan shutdown hook at `server.py:20`. A hook there fires only after the 6 s drain it was meant to cut short. The abort must happen when the signal arrives.

**Interfaces:**
- Produces: in `streaming.py`, `abort_all_streams() -> int` (sets the `stop` event of every in-flight `threaded_stream`, returns how many; `threaded_stream` registers its `stop` event in a module-level set guarded by a lock on entry and removes it in its `finally`). In `main.py`, `class MagniteServer(uvicorn.Server)` overriding `handle_exit(self, sig, frame)` to call `abort_all_streams()` and then `super().handle_exit(sig, frame)`; `request_shutdown()` (the `/_desktop/shutdown` path) calls `abort_all_streams()` too. Both engines that stream (`llamacpp`, `torch`) already go through `threaded_stream`, so no engine changes are needed; confirm with `grep -rn threaded_stream` that no other engine streams on its own.

- [ ] **Step 1: Write failing tests:** (a) a `threaded_stream` over a slow producer (one token per 50 ms, 10 s total): start consuming, call `abort_all_streams()`, assert the stream ends within 1 s and the return value is 1; (b) `MagniteServer.handle_exit` calls `abort_all_streams` once (patch it) before delegating to uvicorn.
- [ ] **Step 2: Targeted run** (`Finding12`). Expected: FAIL.
- [ ] **Step 3: Implement** the registry, `abort_all_streams`, `MagniteServer`, and the `request_shutdown` call.
- [ ] **Step 4: Targeted run** (`Finding12`) and full suite. Expected: PASS.
- [ ] **Step 5: Re-break (backend).** `break_recheck.py --only 12 --gguf <path>` (SIGTERM mid-stream). Expected: exit in < 2 s (was 6.2 s) and the client sees a normally terminated, truncated stream.
- [ ] **Step 6: Observe the GUI half before changing it.** The report read the 15 s `waitForFinished` in `BackendManager::stop()` from code and never ran the GUI. `MainWindow` has no `closeEvent`; the backend is stopped when `BackendManager` is destroyed after the event loop ends, by which time the window may already be gone. Run `MAGNITE_HOME=<scratch> ./build/magnite-app`, start a long generation, press Cmd+Q, and watch whether the window stays on screen frozen. If it does, hide it first (call `hide()` at the top of `~MainWindow`, before the backend is torn down); if it does not, change nothing in C++ and record "GUI freeze not reproducible; the lingering process was the only symptom, fixed by Step 3".

---

### Task 12: #13 Chat history: silent skips and launch cost

**Files:**
- Modify: `app/src/panels/chatstore.h/.cpp` (constructor at `chatstore.cpp:26`), `app/src/panels/chathistorycontroller.cpp` (notice display), UI check suite (grep `ChatStore` under `app/src/testing/`)
- Test: UI checks run with the binary's check flag (same one the existing ChatStore tests use)

**Interfaces:**
- Produces: `QStringList ChatStore::unreadableIds() const` (ids skipped at load; files are still left on disk, as today). The history UI shows one non-modal line "N chat(s) could not be read" when non-empty.

- [ ] **Step 1: Measure first (the report did not verify the cost).** Write a throwaway script in the scratchpad that generates 2,000 valid chat files (copy the JSON shape written by `ChatStore::save`) plus 3 corrupt ones; time `ChatStore` construction via a UI-check case. Record the number. Decision rule: implement the summary index (Step 4) only if construction exceeds 300 ms; otherwise note "measured, not an issue" in the plan.
- [ ] **Step 2: Write failing UI checks:** with 3 corrupt files and 5 valid ones, `unreadableIds().size() == 3`, the list has 5 entries, and the corrupt files are untouched on disk.
- [ ] **Step 3: Implement** `unreadableIds()` and the history notice.
- [ ] **Step 4 (conditional): Summary index.** If Step 1 exceeded 300 ms: persist `chats/index.json` mapping `id → {summary, mtime, size}`; at launch parse only files whose mtime/size differ; rebuild the entry on save/rename/delete. Add a check that a stale index (file edited externally) is re-read.
- [ ] **Step 5: Build and run** `./build/magnite-app --ui-check`. Expected: `0 failure(s)` (use the same invocation the existing `ChatStore` checks run under; they live in `app/src/testing/uicheck.cpp`).
- [ ] **Step 6: Re-break.** Re-run the Step 1 generator with corrupt files: notice is visible in the history list (screenshot via `./build/magnite-app --screenshot <dir>`), and the measured launch time is recorded.

---

### Task 13: Full re-break and regression sweep

**Files:** none modified.

- [ ] **Step 1: All probes.** `.venv/bin/python docs/testing/break_recheck.py --gguf <path>`. Expected: `HELD` for #1,2,3,4,5,6,7,8,9,10,11,12; `probe_2b` is `HELD` if break-02 Phase B was approved and done, otherwise `SKIP (Phase B not started)` (record which); `MANUAL` for #13 (walked through in Task 12); exit code 0.
- [ ] **Step 2: Offline suite.** `PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests -v`. Expected: all pass, test count higher than the pre-plan count recorded in Task 0.
- [ ] **Step 3: "Held up" list still holds.** Re-run the report's held-up probes (auth/Host rejection with `MAGNITE_API_TOKEN`, empty/system-only chats, 2 MB message rejected, three concurrent chats, abort releases the lock, cancel under 2 s). Add each as a `HELD`-expected probe if not already in the harness.
- [ ] **Step 4: Client checks.** `./build/magnite-app --theme-check` prints `0 failure(s)`; `./build/magnite-app --ui-check` prints `0 failure(s)`; `./build/magnite-app --restart-policy-check` prints `0 failure(s)`; `MAGNITE_SELFTEST_INFERENCE=1 ./build/magnite-app --selftest` prints `[selftest] PASS (0 failures)`. Run the selftest with `MAGNITE_HOME=<scratch>` and `HOME=<scratch>` set.
- [ ] **Step 5: Update the report.** Append a "Resolution" table to `BREAK_REPORT_2026-11-10.md` (finding, fix summary, test name, probe result, date). Do not edit the original findings text.

---

## Progress notes

(Fill in during execution: baseline offline test count from Task 0, each task's probe before/after result, manual-run results for break-02 Task A1 and Tasks 11, 12, anything that deviated from this plan and why.)
