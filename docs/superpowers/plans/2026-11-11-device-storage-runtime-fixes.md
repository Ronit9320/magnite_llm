# Device, Storage and Runtime-Package Fixes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix three Settings/Training gaps, built and verified one at a time: (1) training "Local compute" card shows real CPU/memory on macOS, (2) Storage & privacy shows a free-space bar, (3) Diagnostics "Installed runtime packages" is redesigned and can install missing packages.

**Architecture:** Backend stays the single source of truth (`telemetry.py`, `routes/system.py`, a new `runtime_packages.py`); C++ widgets only render JSON and expose an `apply…(QJsonObject)` seam so they can be tested with fixture data. Each fix is independent and ships on its own.

**Tech Stack:** Python/FastAPI + `unittest`; C++20/Qt6 widgets; `psutil` (already pinned in all three `packaging/**/constraints-*.txt`); the binary's `--selftest` / `--theme-check` / `--screenshot`.

**Spec:** This conversation. Root-cause analysis for Fix 1: `device_telemetry()` ([telemetry.py](../../../backend/src/backend/training/telemetry.py)) reads utilization only from `nvidia-smi`; the fallback hard-codes `utilization_percent`, `temperature_c` and `power_w` to `None`, and `memory_budget()` ([estimates.py:36](../../../backend/src/backend/training/estimates.py#L36)) returns `available_bytes: None` on macOS.

## Global Constraints

- **No hardcoded hex** in widget code or QSS; add a token to `app/src/theme.h` and extend `Theme::selfCheck()` only if no existing token fits (AGENTS.md).
- New C++ files go in `MAGNITE_SOURCES` in `app/CMakeLists.txt`.
- Backend routes stay loopback + Bearer-protected and obey `UpdateGate`; `/system/*` reads never import an ML runtime.
- Never show a number that was not measured: unknown stays `—` / "Unknown"; partial scans stay lower bounds (existing `≥` convention).
- `/system/info` response keys must not change (`test_system_settings.py` asserts the exact key set). New data goes on new fields/routes.
- Work on branch `fix`. **Leave changes uncommitted** until the user says to commit (standing preference); no push. When committing: one-line subject, no Claude attribution.
- Offline regressions: `PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests -v`; UI: `./build/magnite-app --theme-check` ("0 failure(s)") and `--selftest`.

## Review Focus

1. **Fix 1 – first CPU sample:** `psutil.cpu_percent(interval=None)` returns a meaningless `0.0` on its first call → must be reported as unknown, not "0%".
2. **Fix 1 – NVIDIA machine:** CUDA branch must still win and keep GPU utilization; CPU is additive, not a replacement.
3. **Fix 2 – cache on another volume:** model cache on a different disk than the data directory must not be subtracted from the data volume's capacity bar.
4. **Fix 2 – partial scan / unknown free space:** incomplete scans must not inflate "other used"; `free_bytes: null` must degrade to today's bar, not draw a full/empty one.
5. **Fix 3 – unsupported/dependent packages:** vLLM on macOS/Windows and PEFT-without-torch must show a reason and no active Install button; the install endpoint must refuse arbitrary package names (allowlist only), concurrent installs, and installs while a model is loaded or training runs.

---

## Part 1 — Training compute card: real CPU + memory (macOS and any non-NVIDIA host)

### Task 1.1: Backend telemetry gains CPU and macOS memory

**Files:**
- Modify: `backend/src/backend/training/telemetry.py`, `backend/src/backend/training/estimates.py:36-38`, `backend/pyproject.toml` (add `psutil>=5.9` to base `dependencies`, not `[ml]`), `packaging/requirements-cpu.txt`, `packaging/macos/requirements-macos-arm64.txt`, `packaging/windows/requirements-windows-x86_64.txt` (psutil is already locked in each constraints file; add as a direct requirement only)
- Test: `backend/tests/test_training_device_telemetry.py`

**Interfaces:**
- Produces: `device_telemetry()` dict gains `cpu_percent: float | None` (0–100, system-wide, since the previous call) and `cpu_count: int | None` on **both** the NVIDIA and fallback branches. On the fallback branch with psutil available, `used_bytes` is `total - available` on macOS. Fallback `detail` becomes `"GPU utilization is not exposed on this system. CPU and memory are system-wide."`
- Consumes: nothing from other tasks.

- [ ] **Step 1: Write failing tests** in `DeviceTelemetryTests`:
  - `test_first_cpu_sample_is_unknown_then_measured`: patch `psutil.cpu_percent` → `[0.0, 37.5]`, reset module state, two `device_telemetry()` calls on the fallback path; assert `cpu_percent is None` then `== 37.5`.
  - `test_cpu_is_reported_alongside_nvidia_gpu`: reuse the `nvidia-smi` fixture; assert `utilization_percent == 94` **and** `"cpu_percent" in data`.
  - `test_macos_memory_used_comes_from_psutil`: patch `sys.platform="darwin"`, `subprocess.check_output`→`b"8589934592"`, `psutil.virtual_memory`→`available=2*1024**3`; assert `memory_budget()["available_bytes"] == 2*1024**3` and telemetry `used_bytes == 6*1024**3`.
  - `test_missing_psutil_leaves_cpu_and_macos_memory_unknown`: make `import psutil` raise `ImportError`; assert `cpu_percent is None`, `used_bytes is None`, no exception.
- [ ] **Step 2: Run** `PYTHONPATH=backend/src .venv/bin/python -m unittest backend.tests.test_training_device_telemetry -v` → new tests FAIL (key missing / `None`).
- [ ] **Step 3: Implement** `_cpu_percent() -> float | None` in `telemetry.py` (lazy `import psutil` inside `try/except ImportError`; module flag `_cpu_primed` makes the first call return `None`; the 3 s UI poll then gives a 3 s average). Add the two fields to both return dicts. In `memory_budget()` darwin branch set `available_bytes` from `psutil.virtual_memory().available` (lazy import, `None` on failure).
- [ ] **Step 4: Run** the full file plus `test_training_estimates.py` → PASS.

### Task 1.2: Training panel shows the right utilization metric

**Files:**
- Modify: `app/src/panels/trainingpanel.cpp:310-317` (`metricRow` keeps its title label reachable via `m_computeValues["utilizationTitle"]`), `:1608-1635`; `app/src/panels/trainingpanel.h` (declare `void applyTelemetry(const QJsonObject &data);` as a test seam and make `refreshTelemetry()` call it)
- Test: `app/src/testing/uicheck.cpp` (new checks beside the existing training checks, using fixture JSON)

**Interfaces:**
- Consumes: Task 1.1 fields `cpu_percent`, `utilization_percent`, `accelerator`.
- Rule: if `utilization_percent` is a number → title "GPU utilization", that value; else if `cpu_percent` is a number → title "CPU utilization", that value; else title "GPU utilization", "—". The 32-segment bar lights from the same value. Tooltip stays `detail`. The "Memory allocation" row already renders `used_bytes`, so macOS gets a real figure with no further change.

- [ ] **Step 1: Write failing checks** in `uicheck.cpp`: feed `applyTelemetry` three fixtures (NVIDIA `{utilization_percent:94,cpu_percent:10}`; Mac `{accelerator:"SYSTEM",cpu_percent:40,used_bytes:6e9,total_bytes:8e9}`; empty `{}`) and assert title/value text, number of lit `trainingUtilBar` segments (30, 13, 0), and memory text `"5.6 GB / 7.5 GB"`.
- [ ] **Step 2: Build and run** `cmake --build build -j && ./build/magnite-app --selftest` → new checks FAIL (no `applyTelemetry`).
- [ ] **Step 3: Implement** the refactor + rule above.
- [ ] **Step 4: Verify** `--theme-check` prints `0 failure(s)`; `--selftest` passes; `--screenshot <dir>` and look at the training panel card with the real backend on this Mac during an actual training run (`MAGNITE_SELFTEST_TRAINING=1 ./build/magnite-app --selftest` for the run; screenshot while it trains). Report what the card shows.

**Added after live verification:** (a) `_cpu()` is throttled to a ≥ 1 s window and shares its last value between callers, because the training panel and Settings both call `device_telemetry()` and a few-millisecond window read as 0%; (b) a measured value above 0 always lights ≥ 1 of the 32 segments; (c) dev-only `MAGNITE_SCREENSHOT_DELAY_MS` / `MAGNITE_SCREENSHOT_DWELL_MS` let `--screenshot` capture panels with live backend data.

**Out of scope (noted, not planned):** Apple-GPU utilization (`powermetrics` needs root) and per-process training-worker CPU/RSS. Say so if you want either.

---

## Part 2 — Storage & privacy: free-space bar

### Task 2.1: `/system/storage` reports disk capacity

**Files:**
- Modify: `backend/src/backend/routes/system.py` (replace `_free_space` with `_disk_usage(path) -> dict | None`)
- Test: `backend/tests/test_system_settings.py`

**Interfaces:**
- Produces: response keeps `free_bytes` and adds `disk_total_bytes: int | None` and `cache_on_data_volume: bool` (compare `os.stat(...).st_dev` of the existing ancestor of each path; `True` when they match or locations are combined).

- [ ] **Step 1: Write failing tests:** `test_storage_reports_disk_total_and_free` (patch `shutil.disk_usage` → `(100, 40, 60)`; assert `disk_total_bytes == 100`, `free_bytes == 60`); `test_storage_flags_cache_on_another_volume` (patch `os.stat` so `st_dev` differs; assert `cache_on_data_volume is False`); `test_unreadable_disk_usage_is_null` (`OSError` → both `None`, no exception).
- [ ] **Step 2: Run** `... unittest backend.tests.test_system_settings -v` → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the file → PASS.

### Task 2.2: Capacity bar in `StorageSummary`

**Files:**
- Modify: `app/src/widgets/settingssummary.cpp` (`StorageDistribution`, `StorageSummary::setMeasurements/setUnavailable`), `.h`; `app/src/theme.h` + `Theme::selfCheck()` only if no existing token fits "other used"
- Test: `app/src/testing/settings_tests.cpp`

**Interfaces:**
- Consumes: Task 2.1 fields.
- Bar segments, left→right: **Models** (only if `cache_on_data_volume`), **Workspace**, **Other used** (`disk_total − free − measured`, clamped ≥ 0; labelled "Other & unscanned" when either scan is incomplete), **Free** (track colour). Legend/caption: `"Free 120 GB of 500 GB"`. If `free_bytes` or `disk_total_bytes` is null, fall back to today's size-split bar with no capacity claim. Update the accessible description (it currently says "not disk capacity").
- Test seam: `StorageSummary::setMeasurements(QJsonObject)` is already public; assert the legend text, the `m_free` label, and `StorageDistribution` segment values via a small `segments()` accessor.

- [ ] **Step 1: Write failing checks** for: full data (4 segments, legend text), `cache_on_data_volume:false` (no model segment), incomplete scan (label "Other & unscanned"), null free (fallback, no "Free … of …" text).
- [ ] **Step 2: Build + `--selftest`** → FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Verify:** `--theme-check` 0 failures, `--selftest` pass, `--screenshot` Settings → Storage & privacy and look at it against the real `~/.magnite`.

---

## Part 3 — Diagnostics: redesigned runtime packages + install missing

### Task 3.0: Spike — can packages be installed at runtime? (decision gate, no product code)

The packaged runtime runs `python -I` from a signed bundle and was built with `uv`; pip may not be present, and writing into the bundle would break its signature and be wiped by updates. Proposed design: install into a user-writable `<app_home>/packages` (`--target`) and **append** that directory to `sys.path` at backend start (bundled copies win; only missing packages come from there).

- [ ] **Step 1:** On this Mac check (a) does the packaged python have `pip`/`ensurepip`; if not, can a pinned `uv` be shipped or can `ensurepip` be used; (b) `--target` + explicit `sys.path` insertion imports a pure-Python and a native-extension wheel under `-I`; (c) `codesign --verify --deep --strict` still passes afterwards (`packaging/macos/verify_installed.sh`).
- [ ] **Step 2:** Write findings in `docs/development/` (short) and **stop for your decision** — the install mechanism (and whether macOS can offer anything at all: vLLM does not support Apple silicon, so on this Mac the only "not installed" package is one that cannot run) depends on this.

### Task 3.1: Package catalogue + status endpoint

**Files:**
- Create: `backend/src/backend/runtime_packages.py`
- Modify: `backend/src/backend/routes/system.py` (new `GET /system/packages`; leave `/system/info` untouched)
- Test: create `backend/tests/test_runtime_packages.py`

**Interfaces:**
- Produces: `CATALOGUE: tuple[Package, ...]` (frozen dataclass: `id, label, role, distribution, requires: tuple[str,...], size_hint: str`) for `llama.cpp`, `vLLM`, `torch`, `PEFT`; `supported(pkg) -> tuple[bool, str | None]` (vLLM: Linux x86_64 only; PEFT needs torch); `status() -> list[dict]` with `{id, label, role, installed_version, state: "installed"|"missing"|"unsupported"|"blocked", reason, size_hint}` where `blocked` = missing prerequisite.
- Test: all installed; each missing; vLLM `unsupported` on `Darwin` with a reason; PEFT `blocked` when torch missing; response contains no paths or environment.

- [ ] Steps: failing tests → run (FAIL) → implement → run (PASS).

### Task 3.2: Install job (allowlisted, single, cancellable)

**Files:**
- Modify: `backend/src/backend/runtime_packages.py`, `backend/src/backend/routes/system.py`
- Test: `backend/tests/test_runtime_packages.py`, access test in `test_system_settings.py` style (credential + `UpdateGate`)

**Interfaces:**
- Produces: `POST /system/packages/{id}/install` → 202 `{job}`; 404 unknown id; 409 when `unsupported`/`blocked`, when another install is running, or when a model is loaded / training is active. `GET /system/packages/install` → `{id, state: "idle"|"running"|"done"|"failed"|"cancelled", log_tail}` (log redacted with `redact_log`, ≤ 4 KB); `POST /system/packages/install/cancel`. The installer command is built only from the catalogue (never from request data), pinned to the packaged constraints file when present, using the mechanism chosen in 3.0.
- Test (installer subprocess mocked): allowlist rejects `"../x"`/unknown ids; second concurrent install → 409; failure surfaces `failed` with redacted tail; cancel terminates the process; endpoint unreachable without the desktop credential.

- [ ] Steps: failing tests → run (FAIL) → implement → run (PASS) → full backend suite green.

### Task 3.3: Redesigned runtime-packages UI + Install action

**Files:**
- Modify: `app/src/widgets/settingssummary.cpp/.h` (`DiagnosticsSummary`: replace the four plain rows with package cards), `app/src/panels/settingspanel.cpp:630-650` (fetch `/system/packages`, POST install, poll job), `app/resources/theme/console.qss` (`@token` placeholders only)
- Test: `app/src/testing/settings_tests.cpp`

**Interfaces:**
- Consumes: Tasks 3.1–3.2 JSON. Add `void DiagnosticsSummary::setPackages(const QJsonArray&)` and `signal installRequested(QString id)`; the panel owns the network calls.
- Layout per package (card): name + one-line role ("Chat engine for GGUF models", "Training, RAG and adapters"), a status pill (`Installed · 2.11.0` / `Not installed` / `Not available on macOS` / `Needs PyTorch`), size hint, and an **Install** button only for `missing`; `unsupported`/`blocked` show the reason and no button. Installing shows an indeterminate progress bar + Cancel; failure shows the redacted tail with Retry. Keep the "installed ≠ compatible" hint. Install requires a confirm dialog stating size and that it downloads from the network.
- Checks: fixture JSON for each state → pill text, button visibility/enabled, accessible names; clicking Install emits the id; `unsupported` has no button.

- [ ] Steps: failing checks → build + `--selftest` (FAIL) → implement → `--theme-check` 0 failures + `--selftest` pass → `--screenshot` the Diagnostics page; with a source build, install a missing package end to end in a scratch `MAGNITE_HOME` and confirm it flips to Installed after refresh.

---

## Part 4 — Remove the small helper text above/below Settings headings

### Task 4.1: Settings window headings carry only the title

**Files:**
- Modify: `app/src/panels/settingspanel.cpp` — delete the `"Make it yours."` hint under the nav title (:161), the `"YOUR WORKSPACE"` eyebrow (:191), the per-page `m_description` label (:193-194, :580) and the `descriptions` list (:52-59); `app/src/panels/settingspanel.h` drop `m_description`; `app/resources/theme/console.qss` drop any rule left unused (keep `settingsHint`/`settingsEyebrow` if other pages still use them).
- Test: `app/src/testing/settings_tests.cpp`

**Interfaces:** none produced or consumed. Page-level helper copy that explains a *control* (row hints, notes) stays; only heading-adjacent taglines go. Out of scope unless you say otherwise: the same pattern in other panels (e.g. Training studio's "Your next iteration starts with what you learned.", "READY TO TRAIN", the sidebar's "Your machine. Your models.") and the "QUICK REFERENCE" eyebrow in the shortcuts card.

- [ ] **Step 1: Write failing check:** for each of the 8 categories, selecting it leaves the settings window with no `QLabel#settingsEyebrow` in the heading area, no label whose text matches the old tagline strings, and a non-empty title; the nav shows no `"Make it yours."` label.
- [ ] **Step 2: Build + `--ui-check`** → new check FAILS.
- [ ] **Step 3: Implement** the deletions above; tighten heading spacing (`headingCopy->setSpacing`, `rightBody->addSpacing(24)`) so the title block still aligns with the close button.
- [ ] **Step 4: Verify:** `--theme-check` 0 failures, `--ui-check` shows no new failures versus the baseline, and `--screenshot` of Settings (page 6) for at least Storage & privacy and Diagnostics.

## Order and gates

1. Part 1 → verify on this Mac (screenshot during a real training run) → you review.
2. Part 2 → verify → you review.
3. Part 4 (small, independent): can go any time after Part 2 since it touches the same page; do it before Part 3 so the redesigned Diagnostics page is built without the removed text.
4. Part 3: run Task 3.0 first and stop for your call; build 3.1–3.3 only after.

## Self-review

- Spec coverage: CPU/memory display, "is it Mac-only" (backend fix is platform-neutral; non-NVIDIA Linux/Windows benefit), free bar, redesigned package list, install-missing → Tasks 1.1–3.3. Gap noted: Apple-GPU utilization intentionally not delivered.
- Type consistency: `cpu_percent`, `disk_total_bytes`, `cache_on_data_volume`, `state` values, `setPackages`/`installRequested` are used identically across tasks.
- Proportion: code blocks are signatures/assertions only.
