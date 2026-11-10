# Break #2 Backend Recovery Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A backend crash no longer ends the session (Phase A), and a training crash no longer takes chat, RAG and the model library down with it (Phase B).

**Architecture:** Phase A: the C++ client relaunches a dead backend with backoff, and the backend saves training progress while a job runs. Phase B: training runs in a child process that follows the existing downloader pattern (`models/pull_worker.py`, supervised from `models/registry.py`): the worker prints one JSON object per stdout line, the backend applies those events to the job, registers the adapter itself, and kills the child on cancel.

**Tech Stack:** C++20/Qt6 (Phase A client), Python 3.13/FastAPI/SQLite/transformers+peft (backend), `unittest`.

**Spec:** `docs/testing/BREAK_REPORT_2026-11-10.md` finding #2, plus the reproduction run on 2026-11-10 (backend `kill -9` leaves the app on "Backend crashed." forever; `kill -9` during training kills chat, RAG and the library together, and the interrupted job comes back as step 0/0 because progress is only saved at start and end). Sibling plan: `2026-11-10-break-report-fixes.md` (findings #1, #3–#13); this plan replaces its former Task 2.

## Global Constraints

- Everything in the sibling plan's Global Constraints applies: **no commits** (memory rule), branch `fix`, scratch `MAGNITE_HOME` for every manual GUI run (a scratch `HOME` does NOT isolate Qt preferences on macOS; see the sibling plan), kill only the backend/worker that this test's app spawned (`pgrep -P <pid>`), never `pkill -f`.
- Phase A runs right after sibling Task 1 (restart must not run before tolerant startup, or a corrupt database burns the restart budget). **Phase B starts only after sibling Task 13 and an explicit go from the user** (Task B0).
- Existing tests patch `backend.training.jobs.run_training` and call `trainer.run_training(job, cfg, path, root, store, cancel)` directly (`test_october_fixes.py`, `test_regressions.py:438`, `test_backend_audit.py:97`). Both must keep working unchanged.
- Training behavior that held up must not change: NaN/inf losses filtered by `record_metrics`, cancel finishing within ~2 s, bad `target_modules` falling back, a traversal-style `base_model` rejected.
- Training limits from sibling Task 6 (`MAX_TOTAL_STEPS = 100_000`, `seed` range) are already in force when Phase B runs.

## Review Focus

1. **Restart wires every panel signal twice**: `MainWindow::onBackendStarted` connects unconditionally. Pinned in A1 Step 7.
2. **A pending restart fires during quit**: the retry timer must be stopped by `stop()`/`stopForUpdate()`. Pinned in A1 Step 8.
3. **A worker orphaned by a backend crash keeps training and filling RAM**: it must exit when the backend dies. Pinned in B4.
4. **Cancel racing the final "adapter ready" message**: a cancelled job must never register an adapter. Pinned in B3.
5. **Worker exits 0 without printing a result** (or prints garbage): the job must end `error`, not stay `running` forever. Pinned in B3.

---

## Phase A: relaunch and keep progress (runs after sibling Task 1)

### Task A1: Backend auto-restart with backoff

**Files:**
- Create: `app/src/restart_policy.h` (header-only), `app/src/testing/restart_tests.cpp` + `.h` (follow `update_tests.cpp`; add both to `MAGNITE_SOURCES` in `app/CMakeLists.txt`)
- Modify: `app/src/main.cpp` (new `--restart-policy-check` flag next to `--theme-check`, line ~53), `app/src/backend_manager.h/.cpp` (`onProcessFinished` at `backend_manager.cpp:281`, `stop()`, `stopForUpdate()`), `app/src/mainwindow.cpp` (`onBackendError` ~477, `onBackendStarted` ~447, `setPillState` at 365)

**Interfaces:**
- Produces: `struct RestartPolicy { int maxAttempts = 5; int baseDelayMs = 1000; std::optional<int> nextDelayMs(); void noteHealthy(); void reset(); int attempts() const; }`. `nextDelayMs()` returns 1000, 2000, 4000, 8000, 16000, then `std::nullopt`. `noteHealthy()` and `reset()` zero the attempt count. `BackendManager` gains `signal restarting(int attempt, int delayMs)`.

- [ ] **Step 1: Write failing policy checks** in the new check flag: the delay sequence above; after two delays `noteHealthy()` makes the next delay `1000`; `reset()` zeroes `attempts()`.
- [ ] **Step 2: Build and run** `./build/magnite-app --restart-policy-check`. Expected: build error (header missing), then with a stub, FAIL.
- [ ] **Step 3: Implement `RestartPolicy`**; run the check. Expected: `0 failure(s)`.
- [ ] **Step 4: Wire `BackendManager`.** In `onProcessFinished`, for an unexpected exit that is not `kExitDataDirInUse` and not `m_stopping`: ask the policy; on a delay emit `restarting(attempt, delay)` and start a member single-shot `QTimer` that calls `start()`; on `nullopt` emit `errorOccurred(Crashed)` (existing modal path). After each successful health probe start a 60 s single-shot timer calling `policy.noteHealthy()`; stop it when the process finishes. `stop()` and `stopForUpdate()` stop the restart timer. Bypass the policy in self-test mode (a crash there must still print `[selftest] backend error` and fail).
- [ ] **Step 5: Wire `MainWindow`.** On `restarting`: `m_backendClient->abortAll()`, clear base URL/token, `setPillState("starting", "Reconnecting")` (any state other than `ok`/`err` already gets the neutral look: no new QSS or theme token) plus a "Reconnecting to Magnite’s local service…" tooltip branch. No modal. Add `Qt::UniqueConnection` to every `connect(...)` in `onBackendStarted` (convert lambda connections to named slots). After a restart, refresh `ChatPanel::refreshModels`, `ChatPanel::refreshEngineStatus`, `AdaptersPanel::refresh`.
- [ ] **Step 6: Re-break (manual).** Reproduce exactly as before (before-state: pill "Backend crashed.", no relaunch after 12 s): `HOME=<scratch> MAGNITE_HOME=<scratch> ./build/magnite-app &`, wait for "Connected", `kill -9 $(pgrep -P <app pid>)`. Expected: pill "Reconnecting", a new child within ~2 s, pill "Connected", no dialog. Then kill the child five more times, each after it reappears: the "Backend crashed." dialog appears only on the sixth crash. Capture the window with `screencapture -x -o -l<id>` before and after; get `<id>` from a small Swift helper saved as `docs/testing/winid.swift` (git-ignored with the rest of `docs/testing/`) that lists `CGWindowListCopyWindowInfo` entries for the app's pid and prints the window number (the same helper used in the 2026-11-10 reproduction).
- [ ] **Step 7: Regression:** after a restart send one chat message; the reply arrives once.
- [ ] **Step 8: Quit during a pending restart (Review Focus 2).** Kill the child, then quit the app (Cmd+Q) within the 1 s before the retry fires. Expected: the app exits, and 10 s later no process remains whose command line contains the scratch `MAGNITE_HOME` path (`pgrep -f <scratch path>` prints nothing): the retry timer did not launch a backend after quit.

### Task A2: Save training progress while a job runs

**Files:**
- Modify: `backend/src/backend/training/jobs.py` (`__init__` recovery at lines 39-46, `_run_queue` at line 61)
- Test: `backend/tests/test_break_fixes_2026_11.py::Break2ProgressPersistence`

**Interfaces:**
- Produces: `TrainingJobManager._PROGRESS_SAVE_SECONDS = 5.0`; while a job runs, `_run_queue` starts a daemon thread that calls `self._persist(job)` every `_PROGRESS_SAVE_SECONDS` until the job finishes (stopped via an `Event`). Recovery keeps `current_step`, `total_steps`, `progress`, `loss` and `history` and sets `message = f"Training interrupted at step {current_step}/{total_steps} by a backend shutdown or crash. Submit a new job to retry."` No checkpoints exist (`save_strategy="no"`), so there is no resume; the message says so by saying "retry".

- [ ] **Step 1: Write failing tests:** (a) with `_PROGRESS_SAVE_SECONDS` patched to 0.05 and a fake `run_training` that sets `job.current_step = 7` and then blocks on an `Event` the test controls: reading `training_jobs.db` while it is blocked shows `current_step == 7` and status `running`; (b) constructing a new manager on that database marks the job `error` with `current_step == 7` and a message containing "step 7"; (c) the saver thread stops after the job ends (no `threading.enumerate()` entry named `training-progress-save` remains).
- [ ] **Step 2: Targeted run** (`Break2ProgressPersistence`). Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Targeted run + full suite.** Expected: PASS.
- [ ] **Step 5: Re-break.** Run the reproduction again (`break_recheck.py --only 2`: load a chat model, start a training job, wait for step ≥ 3, `kill -9` the backend, relaunch it). Before: `step 0/0`. Expected now: `status='error'`, `current_step >= 3`, message names the step. Chat/RAG/library still die with the process in Phase A; that is expected until Phase B.

---

## Phase B: training in a child process (starts only after the user's go)

### Task B0: Go/no-go checkpoint

- [ ] **Step 1:** After sibling Task 13, show the user the sibling plan's resolution table and A1/A2 results, and ask whether to proceed with Phase B. Do not start B1 without an explicit yes. If the answer is "defer", Phase A stands as the fix for #2 and this plan is closed with that note in its progress section.

### Task B1: Split `run_training` into train / publish (behavior-preserving)

**Files:**
- Modify: `backend/src/backend/training/trainer.py:208-436`
- Test: `backend/tests/test_backend_audit.py` and `test_regressions.py` must pass unchanged; new `Break2TrainerSplit` tests

**Interfaces:**
- Produces:
  - `@dataclass class TrainOutcome: adapter_dir: str; architecture: str; size_mb: float; global_step: int; total_steps: int; training_loss: float | None; resolved_base: str`
  - `train_lora(cfg: TrainingConfig, dataset_path: str, adapter_dir_root: str, job_id: str, report: Callable[[dict], None], cancel: threading.Event) -> TrainOutcome` (everything up to and including `save_pretrained`; no `TrainingJob`, no store; raises `_Cancelled` or the underlying exception). `report` receives `{"status": "running", "message": str, "progress": float}`, `{"total_steps": int}`, and `{"log": {"step": int, "logs": dict, "elapsed": float, "epoch": float | None}}`.
  - `apply_event(job: TrainingJob, event: dict) -> None` (moves the `_ProgressCallback`/`job.message`/`record_metrics` logic onto the job; `record_metrics` and its NaN filter stay here, parent-side).
  - `publish_adapter(job, cfg, outcome, adapter_store, cancel) -> None` (the existing `AdapterRecord` build plus the `publication_lock` block).
  - `run_training(...)` keeps its exact signature and becomes `train_lora` + `apply_event` + `publish_adapter` in one thread.

- [ ] **Step 1: Write failing tests:** `train_lora` and `publish_adapter` exist; `apply_event` with `{"log": {"step": 3, "logs": {"loss": float("nan")}, ...}}` leaves `job.loss` unchanged and adds nothing to `job.history` (the held-up NaN behavior); with a finite loss it records it.
- [ ] **Step 2: Targeted run.** Expected: FAIL (names missing).
- [ ] **Step 3: Refactor** without changing observable behavior. Run `test_backend_audit`, `test_regressions`, `test_october_fixes`: Expected PASS before moving on.
- [ ] **Step 4: Run** `MAGNITE_SELFTEST_TRAINING=1 ./build/magnite-app --selftest` (scratch `HOME`/`MAGNITE_HOME`). Expected: `[selftest] PASS (0 failures)`.

### Task B2: The worker process

**Files:**
- Create: `backend/src/backend/training/train_worker.py`
- Test: `Break2Worker`

**Interfaces:**
- Produces: `python -m backend.training.train_worker <spec.json>` where the spec is `{"cfg": TrainingConfig dict, "dataset_path": str, "adapter_dir_root": str, "job_id": str}`. `main(argv: list[str] | None = None, out=None, stdin=None) -> int`. stdout protocol, one JSON object per line (anything else ignored by the parent): the three `report` event shapes from B1, then exactly one of `{"result": "ok", "outcome": TrainOutcome as dict}` / `{"result": "cancelled"}` / `{"error": str}`. A thread reads stdin; the line `cancel` sets the cancel event. Parent-death watch: import and reuse `backend.models.pull_worker._start_parent_watch` (do not copy it). Exit codes: 0 ok or cancelled, 1 error, 2 bad usage.

- [ ] **Step 1: Write failing tests** (in-process `main` with patched `train_lora`): result line printed and exit 0; patched `train_lora` raising → one `{"error": ...}` line and exit 1; writing `cancel\n` to the fake stdin sets the event the patched `train_lora` receives; bad usage → exit 2.
- [ ] **Step 2: Targeted run.** Expected: FAIL.
- [ ] **Step 3: Implement.** Flush stdout after every line.
- [ ] **Step 4: Targeted run.** Expected: PASS.

### Task B3: Supervise the worker from the job manager

**Files:**
- Create: `backend/src/backend/training/runner.py`
- Modify: `backend/src/backend/training/jobs.py:14,70` (rebind `run_training`)
- Test: `Break2Runner`

**Interfaces:**
- Produces: `_default_train_command(spec_path: str) -> list[str]` (same flags logic as `models/registry._default_pull_command`: `-B`, `-I` when `sys.flags.isolated`, module `backend.training.train_worker`); `describe_exit(returncode: int) -> str` (negative codes are signals: `-9` → "The training process was killed by the system, most likely because the machine ran out of memory. Close other apps or choose a smaller model or batch size."; other negatives → "The training process crashed (signal N)."; positive → "The training process exited unexpectedly (code N)."); `run_training_isolated(job, cfg, dataset_path, adapter_dir_root, adapter_store, cancel) -> None` with the same signature as `run_training`. It writes the spec to a temp file, starts the worker with `stdin=PIPE, stdout=PIPE, stderr=STDOUT` (as the downloader does), applies each event with `apply_event`, and on `result: ok` calls `publish_adapter` (which re-checks `cancel` under `publication_lock`). While running, a cancel (the event is set) writes `cancel\n` to the worker's stdin; if it is still alive after 10 s it is killed. A worker that exits without a result line, or with a non-zero code and no `error` line, ends the job `error` with `describe_exit`. `jobs.py` replaces its import with `from backend.training.runner import run_training_isolated as run_training` (existing tests patch `backend.training.jobs.run_training`, so the name stays).

- [ ] **Step 1: Write failing tests** using a tiny fake worker script (`python -c`/temp file) selected by patching `_default_train_command`: (a) worker emits two `log` events, a `result` → job `done`, adapter registered, `current_step` advanced; (b) worker `os.kill(os.getpid(), SIGKILL)` mid-run → job `error` with the "ran out of memory" message, backend object unaffected (a second job queued afterwards still runs); (c) worker exits 0 with no result (Review Focus 5) → job `error`; (d) cancel set before the result line → job `cancelled`, no adapter registered (Review Focus 4), temp spec file removed; (e) worker ignores `cancel` → killed after the (patched, 0.2 s) grace period.
- [ ] **Step 2: Targeted run.** Expected: FAIL.
- [ ] **Step 3: Implement** `runner.py` and the one-line rebind in `jobs.py`.
- [ ] **Step 4: Targeted run + full suite.** Expected: PASS.

### Task B4: Shutdown and orphan handling

**Files:**
- Modify: `backend/src/backend/training/runner.py`, `backend/src/backend/training/jobs.py` (`shutdown`, line 80)
- Test: `Break2WorkerLifecycle`

**Interfaces:**
- Produces: `runner.terminate_all(grace: float = 5.0) -> None` (module-level registry of live worker `Popen`s, same pattern as sibling Task 11's stream registry; sends `cancel`, waits, then kills). `TrainingJobManager.shutdown` calls it before joining the queue thread.

- [ ] **Step 1: Write failing tests:** with a fake worker that sleeps 60 s: `shutdown()` returns within `grace + 1` s and the worker is gone; a worker started with its parent killed with `-9` exits within 3 s (spawn a helper parent process that launches the real `train_worker` with a stub spec, kill the helper, assert the worker pid disappears; this is Review Focus 3).
- [ ] **Step 2: Targeted run.** Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Targeted run + full suite.** Expected: PASS.

### Task B5: Memory warning when training starts with a chat model loaded

**Files:**
- Modify: `backend/src/backend/training/schemas.py` (`TrainingJob.warnings: list[str] = []`), `backend/src/backend/routes/training.py:56` (`start`), the training panel's status line in `app/src/panels/trainingpanel.cpp`
- Test: `Break2MemoryWarning`

**Interfaces:**
- Produces: `POST /training/start` response carries `warnings: ["A chat model is loaded and shares memory with training. If training is killed, unload the chat model first."]` when `get_supervisor().status().loaded_model` is set; empty otherwise. The panel shows warnings in the existing `statusLabel[state="warn"]` style (no new theme token). Start is never refused.

- [ ] **Step 1: Write failing tests:** with a fake engine loaded the response has one warning; with nothing loaded `warnings == []`.
- [ ] **Step 2: Targeted run.** Expected: FAIL.
- [ ] **Step 3: Implement backend and panel.**
- [ ] **Step 4: Targeted run; `./build/magnite-app --ui-check` and `--theme-check` print `0 failure(s)`.**

### Task B6: Packaged builds, selftests and the final re-break

**Files:** none new; verification only.

- [ ] **Step 1: Source build.** `MAGNITE_SELFTEST_TRAINING=1` and `MAGNITE_SELFTEST_ADAPTER=1 ./build/magnite-app --selftest` (scratch `HOME`/`MAGNITE_HOME`): `[selftest] PASS (0 failures)` each.
- [ ] **Step 2: Packaged runtime.** Follow `packaging/README.md` for the macOS bundle and confirm the worker starts from `runtime/python` (the `-I`/isolated flags path in `_default_train_command`), then run one real training job in the packaged app. Record the result; if packaging cannot be run here, say so explicitly rather than claiming it works.
- [ ] **Step 3: Re-break (the reproduction, changed target).** `break_recheck.py --only 2`: start a chat model and a training job, `kill -9` the **worker** (not the backend). Expected: chat still answers (200), library and RAG still answer, the job is `error` with the "ran out of memory" message, and a new training job can start. Then `kill -9` the **backend** mid-training: the worker must be gone within 3 s (no orphan in `pgrep -f train_worker`).
- [ ] **Step 4: Update the report's resolution table** for #2 (Phase A and B results, separately).

---

## Progress notes

(Fill in during execution: A1 manual-run results with screenshots, A2 before/after step counts, the B0 decision and date, any packaging findings.)
