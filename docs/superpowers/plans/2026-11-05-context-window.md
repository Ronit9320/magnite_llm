# Context Window Meter and Auto-Compact Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Show how full the loaded model's context is (Ring / Bar / Ring + banner / Off), warn from 70%, and auto-compact older messages into a model-written summary when the user sends at or above their chosen percentage.

**Architecture:** The backend stays stateless: two new routes (`/inference/context-usage`, `/inference/compact`) built on pure functions in a new `inference/context.py`. The client keeps the full transcript in `ChatPanel::m_history` and adds a small `m_compaction {summary, covered}` that decides what is *sent*. A new `ContextMeter` widget renders the styles; its threshold logic lives in free functions so the fixture-based UI check can test them.

**Tech Stack:** Python 3 / FastAPI / pydantic / unittest + httpx (backend); C++20 / Qt6 Widgets, QSettings, loopback fixture server in `app/src/testing/uicheck.cpp` (client).

**Spec:** `docs/superpowers/specs/2026-11-05-context-window-design.md`

## Global Constraints

- No hex colours in widget code or QSS; add tokens to `app/src/theme.h` and extend `Theme::selfCheck()` (AGENTS.md).
- New C++ files go in `MAGNITE_SOURCES` in `app/CMakeLists.txt`.
- Fill = `(tokens(messages) + max_tokens) / context_size`. Levels: green < 0.70, amber 0.70–0.90, red > 0.90.
- Auto-compact setting: Off, or 70–90 in steps of 5; default 85. Meter style default Ring. Keys: `chat/meterStyle` (`ring|bar|ringBanner|off`), `chat/autoCompactPercent` (0 = off).
- `keep_recent` default 4. Compaction runs at most once per Send.
- Estimated figures are prefixed `~`. Ollama/vLLM keep the 2048 default limit (not fixed here).
- `/inference/context-usage` must NOT take `InferenceSupervisor.operation()`. `/inference/compact` does.
- Error mapping: no model loaded → `RuntimeError` → HTTP 409; bad input → `ValueError` → 400; client disconnect → 499.
- All changes stay local and **uncommitted** (standing user instruction) — every task's "Commit" step is replaced by "Record progress": tick the checkboxes in this file.
- Baseline: `PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests` = 196 tests OK. It must stay green after every backend task.

## Review Focus

1. Send with an empty or one-message history at ≥ threshold (nothing older to summarize) — no compaction, no crash, message still sent.
2. `Response tokens` ≥ context window (e.g. 8192 on a 2048 model) — meter shows over 100% with a "Reduce Response tokens" warning; compaction is never started.
3. Stop pressed during compaction — input text restored, Send usable again, `m_history` and `m_compaction` unchanged.
4. Backend unreachable / 409 during meter refresh or compaction — meter shows nothing or keeps the last value, Send is never left disabled, the chat still sends.
5. Model unloaded or switched mid-chat — meter resets/recomputes; an existing summary is still valid text and is kept.

---

## File Structure

| File | Responsibility |
|---|---|
| `backend/src/backend/inference/context.py` (new) | Pure functions: fill, split, drop-to-fit, summary prompt/message. No FastAPI, no engine I/O. |
| `backend/src/backend/inference/schemas.py` (modify) | Request/response models for the two routes. |
| `backend/src/backend/routes/inference.py` (modify) | The two routes. |
| `backend/tests/test_context_window.py` (new) | Pure-function tests + route tests with `FakeEngine`. |
| `app/src/widgets/contextmeter.{h,cpp}` (new) | `ContextMeter` widget + free functions `levelFor`, `shouldCompact`, `parseMeterStyle`, `parseAutoCompact`. |
| `app/src/theme.h`, `theme.cpp` (modify) | Meter tokens and self-check. |
| `app/src/panels/chatpanel.{h,cpp}` (modify) | Meter placement, usage refresh, compaction flow, divider. |
| `app/src/panels/settingspanel.{h,cpp}` (modify) | "Chat" card with the two settings. |
| `app/src/mainwindow.cpp` (modify) | Connect `settingsChanged` → `ChatPanel::reloadContextSettings`. |
| `app/src/testing/uicheck.cpp` (modify) | Fixture routes + checks. |
| `app/CMakeLists.txt` (modify) | Add new sources. |

---

### Task 1: Pure context functions (backend)

**Files:**
- Create: `backend/src/backend/inference/context.py`
- Test: `backend/tests/test_context_window.py`

**Interfaces:**
- Produces (all in `backend.inference.context`):
  - `fill_of(used: int, reserved: int, limit: int) -> float` — `(used + reserved) / limit`; `limit <= 0` returns `float("inf")`.
  - `split_for_compaction(messages: list[ChatMessage], keep_recent: int) -> int` — number of leading messages to summarize. Start at `max(0, min(len(messages) - keep_recent, len(messages) - 1))` (so the last message is always kept); while that index is > 0 and `messages[index].role != ChatRole.user`, decrement it. Returns 0 if there is nothing older.
  - `drop_to_fit(count: Callable[[list[ChatMessage]], int], messages: list[ChatMessage], previous_summary: str, reserved: int, limit: int, target_fill: float) -> int` — smallest `n >= 0` such that, with `summary_message(previous_summary)` prepended when non-empty, `fill_of(count(prefix + messages[n:]), reserved, limit) < target_fill`; if no `n` achieves it, returns the `n` that leaves exactly the last user message onward (never drops the final message).
  - `summary_message(summary: str) -> ChatMessage` — role `system`, content `"Summary of the earlier conversation: " + summary`.
  - `summarize_prompt(messages: list[ChatMessage], previous_summary: str) -> list[ChatMessage]` — the messages sent to the engine to produce a summary (a `system` instruction, then one `user` message containing the transcript and any previous summary).

- [x] **Step 1: Write failing tests** in `test_context_window.py`, class `PureFunctionTests(unittest.TestCase)`, importing `from backend.inference.context import ...` and `from backend.inference.schemas import ChatMessage, ChatRole`. Helper `msgs(*roles)` builds messages alternating content `"m0"`, `"m1"`… Tests and assertions:
  - `test_fill_of_basic`: `fill_of(700, 100, 1000) == 0.8`; `fill_of(0, 0, 1000) == 0.0`.
  - `test_fill_of_over_and_zero_limit`: `fill_of(900, 500, 1000) == 1.4`; `fill_of(1, 1, 0) == float("inf")`.
  - `test_split_fewer_than_keep_recent`: 3 messages (u,a,u), `keep_recent=4` → `0`.
  - `test_split_keeps_pair_whole`: 8 messages (u,a ×4), `keep_recent=3` → raw index 5 is an assistant → moves to 4 → returns `4`.
  - `test_split_exact_boundary_on_user`: 8 messages, `keep_recent=4` → `4`.
  - `test_split_leading_system_message`: `[system, u, a, u, a, u, a]` (7 messages), `keep_recent=2` → raw index 5 is a user → returns `5`.
  - `test_split_keep_recent_zero_still_keeps_last_message`: 6 messages (u,a ×3), `keep_recent=0` → raw index clamps to 5 (assistant) → moves to 4 → returns `4`; assert result `< len(messages)`.
  - `test_drop_to_fit_drops_just_enough`: counter `lambda ms: sum(len(m.content) for m in ms)`, 6 messages of 100 chars, `reserved=0, limit=1000, target_fill=0.45` → remaining must be < 450 chars → returns `2`.
  - `test_drop_to_fit_counts_previous_summary`: same but `previous_summary` of 200 chars → returns a larger `n` than without it (assert `>`).
  - `test_drop_to_fit_never_drops_last_message`: limit tiny → result `== len(messages) - 1`.
  - `test_summary_message_shape`: role is `system`, content starts with `"Summary of the earlier conversation: "`.
  - `test_summarize_prompt_includes_previous_summary_and_transcript`: both strings appear in the returned messages' text; first message role `system`.
- [x] **Step 2: Run** `PYTHONPATH=backend/src .venv/bin/python -m unittest backend.tests.test_context_window -v` from `backend/tests` (or `-s backend/tests -p test_context_window.py`). Expected: ImportError / FAIL.
- [x] **Step 3: Implement** the five functions in `context.py` per the Interfaces block. `drop_to_fit` is a linear scan from `n = 0` upward (histories are small).
- [x] **Step 4: Run** the new tests; expected all PASS. Then run the full suite; expected 196 + new tests, OK.
- [x] **Step 5: Record progress** (tick this task).

---

### Task 2: `POST /inference/context-usage`

**Files:**
- Modify: `backend/src/backend/inference/schemas.py` (append models)
- Modify: `backend/src/backend/routes/inference.py` (add route)
- Test: `backend/tests/test_context_window.py` (class `UsageRouteTests`)

**Interfaces:**
- Consumes: `fill_of` from Task 1; `engine.count_messages`, `engine.context_size()`.
- Produces:
  - `class ContextUsageRequest(BaseModel)`: `messages: list[ChatMessage]`, `max_tokens: int = Field(default=256, ge=1, le=8192)`.
  - `class ContextUsage(BaseModel)`: `used: int`, `reserved: int`, `limit: int`, `fill: float`, `exact: bool`.
  - Route `POST /inference/context-usage` → `ContextUsage`. `exact` is `True` only when the engine is `TorchPeftEngine` (use `engine.name == "torch"`).
  - Reads `get_supervisor()._engine`; if `None` raise `RuntimeError("No inference engine loaded. Load a model first.")` → 409. Does not call `sup.operation()`.

- [x] **Step 1: Write failing tests** (reuse `fixtures.ApiTests.asyncSetUp/asyncTearDown` exactly as `test_october_fixes.py` does: `import test_regressions as fixtures`; `asyncSetUp = fixtures.ApiTests.asyncSetUp`). `FakeEngine.count_messages` = sum(len+8), `context_size()` = 2048 (base default). Tests:
  - `test_usage_numbers`: post two messages of 10 chars each, `max_tokens=100` → `used == 36`, `reserved == 100`, `limit == 2048`, `fill == (36+100)/2048` (assert with `assertAlmostEqual`), `exact is True` (FakeEngine.name is `"torch"`).
  - `test_usage_no_model_is_409`: set `self.sup._engine = None` → status 409.
  - `test_usage_not_blocked_while_busy`: `with self.sup.operation():` hold the lock, then post → status 200 (proves no lock).
  - `test_usage_empty_messages`: `messages=[]` → 200, `used == 0`.
  - `test_usage_rejects_bad_max_tokens`: `max_tokens=0` → 422.
  - `test_estimate_flag_false_for_other_engines`: set `self.sup._engine.name = "llamacpp"` → `exact is False`.
- [x] **Step 2: Run** the new class; expected FAIL (404).
- [x] **Step 3: Implement** models and route (sync `def`, like `/status`).
- [x] **Step 4: Run** new tests then full suite; expected PASS / all OK.
- [x] **Step 5: Record progress.**

---

### Task 3: `POST /inference/compact`

**Files:**
- Modify: `backend/src/backend/inference/schemas.py`, `backend/src/backend/routes/inference.py`
- Test: `backend/tests/test_context_window.py` (class `CompactRouteTests`)

**Interfaces:**
- Consumes: `split_for_compaction`, `drop_to_fit`, `summary_message`, `summarize_prompt`, `fill_of` (Task 1).
- Produces:
  - `class CompactRequest(BaseModel)`: `messages: list[ChatMessage]`, `previous_summary: str = ""`, `max_tokens: int = Field(default=256, ge=1, le=8192)`, `target_fill: float = Field(default=0.85, gt=0.0, le=1.0)`, `keep_recent: int = Field(default=4, ge=0, le=64)`.
  - `class CompactResult(BaseModel)`: `summary: str`, `covered: int`, `method: Literal["summary", "dropped"]`.
  - Route `POST /inference/compact` → `CompactResult`.
  - Algorithm: `n = split_for_compaction(...)`. If `n == 0`: return `{summary: previous_summary, covered: 0, method: "summary"}` without calling the engine. Otherwise inside `with sup.operation():` generate a summary by consuming `engine.chat_stream(summarize_prompt(messages[:n], previous_summary), max_tokens=256, temperature=0.2, top_p=0.95)` into a string (strip it). Then compute the remaining prompt `[summary_message(summary)] + messages[n:]`; if the summary is non-empty and `fill_of(engine.count_messages(remaining), req.max_tokens, engine.context_size()) < target_fill` return `method "summary"`, `covered n`. Otherwise (engine raised a non-cancel exception, empty summary, or too large) return `method "dropped"`, `summary = previous_summary`, `covered = drop_to_fit(engine.count_messages, messages, previous_summary, max_tokens, engine.context_size(), target_fill)`. Runs the generation on the event loop (async route); wrap in the same disconnect watching used by `_first_token` so a closed connection returns `Response(status_code=499)`.

- [x] **Step 1: Write failing tests.** Add a `SummaryEngine(fixtures.FakeEngine)` whose `chat_stream` yields a configurable string and records the messages it was called with; swap it in with `self.sup._engine = SummaryEngine('test')`. Build a 10-message history of 40-char contents. Tests:
  - `test_summary_used`: summary `"short"` → `method == "summary"`, `covered == split_for_compaction(..., 4)` (compute expected via the Task 1 function and also assert it equals `6`), `summary == "short"`.
  - `test_nothing_to_compact`: 3 messages → `covered == 0`, engine not called, `summary == previous_summary`.
  - `test_engine_error_falls_back_to_dropped`: engine raises `RuntimeError("boom")` mid-stream → `method == "dropped"`, `covered > 0`, `summary == ""`.
  - `test_empty_summary_falls_back`: summary `"   "` → `dropped`.
  - `test_too_large_summary_falls_back`: summary of 5000 chars → `dropped`.
  - `test_previous_summary_given_to_engine_and_kept_on_drop`: `previous_summary="old"`; assert `"old"` appears in the text the engine received; with the empty-summary engine assert response `summary == "old"`.
  - `test_result_below_target`: on `dropped`, recompute fill of `messages[covered:]` with the fake counter and assert `< target_fill` (use `target_fill=0.5`, `max_tokens=100`).
  - `test_no_model_is_409`: `_engine = None` → 409.
  - `test_busy_is_409`: hold `self.sup.operation()` → 409.
  - `test_validation`: `target_fill=0` → 422; `keep_recent=-1` → 422.
- [x] **Step 2: Run**; expected FAIL (404).
- [x] **Step 3: Implement** per the Interfaces algorithm.
- [x] **Step 4: Run** new tests, full suite; expected all OK.
- [x] **Step 5: Record progress.**

---

### Task 4: Theme tokens and `ContextMeter` logic

**Files:**
- Create: `app/src/widgets/contextmeter.h`, `app/src/widgets/contextmeter.cpp`
- Modify: `app/src/theme.h`, `app/src/theme.cpp`, `app/CMakeLists.txt`, `app/src/testing/uicheck.cpp`

**Interfaces:**
- Produces in `contextmeter.h`:
  - `namespace ContextMeterLogic { enum class Level { Ok, Warn, Critical }; enum class Style { Ring, Bar, RingBanner, Off }; Level levelFor(double fill); bool shouldCompact(double fill, int autoCompactPercent); Style parseStyle(const QString &key); QString styleKey(Style); int parseAutoCompact(const QVariant &stored); QString usageText(int used, int reserved, int limit, bool exact); }`
    - `levelFor`: `< 0.70` Ok; `<= 0.90` Warn; else Critical.
    - `shouldCompact(fill, pct)`: `pct` in {70,75,80,85,90} and `fill >= pct/100.0`; `pct == 0` or any other value → `false`.
    - `parseStyle`: `"ring"|"bar"|"ringBanner"|"off"`; anything else → `Style::Ring`.
    - `parseAutoCompact(QVariant)`: valid {0,70,75,80,85,90} → that value; missing/invalid/out-of-range/non-multiple-of-5 → `85`.
    - `usageText(1390, 256, 4096, exact)` → `"1,390 / 4,096 tokens"` (used excludes reserve in the text; reserve shown in tooltip), prefixed `"~"` when `!exact`.
  - `class ContextMeter : public QWidget` with `setStyle(Style)`, `setUsage(int used, int reserved, int limit, bool exact)`, `clearUsage()`, `double fill() const`, `Level level() const`, `QString warningText() const` (empty below 70%; when `reserved >= limit` returns `"Reduce Response tokens"`; at 70–90% returns "Context is getting full."; above 90% returns text saying auto-compact will run, or, when `setAutoCompact(int)` was given 0, that the next message may fail), `QWidget *banner()` (a separate widget shown only for style `RingBanner` and level `Critical`; ring/bar colours change from 70% in all styles except Off), objectNames `contextRing`, `contextBar`, `contextBanner`, `contextBannerNewChat` (button; emits `newChatRequested()`).
  - New theme tokens (functions in `theme.h`, registered in `tokens()` and checked in `selfCheck()`): `meterTrack()`; level colours reuse `statusSuccess/Warning/Error`.

- [x] **Step 1: Write failing checks** in `uicheck.cpp` (new block after the existing checks, before the final failure count) using the existing `check(bool, QString)` lambda:
  - `levelFor`: 0.69→Ok, 0.70→Warn, 0.90→Warn, 0.91→Critical, 1.5→Critical.
  - `shouldCompact`: (0.84,85)→false, (0.85,85)→true, (0.99,0)→false, (0.99,86)→false, (0.70,70)→true.
  - `parseAutoCompact`: `QVariant()`→85, `"abc"`→85, `60`→85, `95`→85, `0`→0, `75`→75.
  - `parseStyle`: `"bar"`→Bar, `""`→Ring, `"garbage"`→Ring, round-trips `styleKey`.
  - `usageText(1390,256,4096,true)=="1,390 / 4,096 tokens"`; `false` → starts with `"~"`.
  - Widget: `ContextMeter m; m.setUsage(1400,256,4096,true)` → `level()==Ok`; `setUsage(3000,256,4096,true)` → Warn; `setUsage(3900,256,4096,true)` → Critical; `setUsage(0,5000,2048,true)` → `warningText()=="Reduce Response tokens"`; `clearUsage()` → `warningText().isEmpty()`.
  - Style visibility (`m.show()` first): Ring → `contextRing` visible, `contextBar` hidden; Bar → inverse; Off → both hidden and `banner()` hidden; RingBanner at Critical → banner visible, at Ok → banner hidden; Ring at Critical → banner hidden.
- [x] **Step 2: Build** (`cmake --build build -j`), expect compile failure (missing header). Add the sources to `MAGNITE_SOURCES` (`src/widgets/contextmeter.cpp`, `.h`).
- [x] **Step 3: Implement** `contextmeter.{h,cpp}` and theme tokens (`meterTrack` dark/light values chosen from the existing surface palette; register in `tokens()`, add `check("meterTrack valid", QColor(...).isValid())` to `selfCheck()`). Painting: Ring is a custom-painted 22×22 arc using `Theme::` colours; Bar is a 4 px track/fill plus a `QLabel` with `usageText`. Tooltip text: `"<pct>% of context used\n<used> of <limit> tokens\n<reserved> reserved for the reply"` (`~` prefixed when estimated).
- [x] **Step 4: Verify** `cmake --build build -j` clean (no warnings from new files with `-Wall -Wextra`); `./build/magnite-app --theme-check` prints `0 failure(s)`; run the UI check (`./build/magnite-app --screenshot /tmp-or-scratch-dir`) and confirm no new FAIL lines.
- [x] **Step 5: Record progress.**

---

### Task 5: Settings card

**Files:**
- Modify: `app/src/panels/settingspanel.h`, `settingspanel.cpp`
- Modify: `app/src/testing/uicheck.cpp`

**Interfaces:**
- Consumes: `ContextMeterLogic::parseStyle/styleKey/parseAutoCompact` (Task 4).
- Produces: new card titled "Chat" with `QComboBox` objectName `meterStyleCombo` (items Ring, Bar, Ring + banner, Off) and `QComboBox` objectName `autoCompactCombo` (items Off, 70%, 75%, 80%, 85%, 90%) plus a hint label. Changes write `chat/meterStyle` and `chat/autoCompactPercent` to `QSettings` immediately and `emit settingsChanged()` (same pattern as `appearanceChanged`, no Save needed). `loadFromSettings()` populates both from QSettings with the Task 4 parsers (defaults Ring / 85).

- [x] **Step 1: Write failing checks** in `uicheck.cpp`: with a clean `QSettings` scope (the check already isolates settings — if not, set `QCoreApplication::setOrganizationName` to a test value first and clear the two keys): a fresh `SettingsPanel` shows `meterStyleCombo` current data `"ring"` and `autoCompactCombo` current text `"85%"`; setting `meterStyleCombo` to Bar writes `QSettings().value("chat/meterStyle")=="bar"`; setting `autoCompactCombo` to Off writes `0`; a stored invalid value (`chat/autoCompactPercent=61`) loads as 85%; a `settingsChanged` spy counts exactly one emission per change.
- [x] **Step 2: Build and run**; expected FAIL (widgets not found).
- [x] **Step 3: Implement** the card inside the existing visual column, after Appearance, using `Surface::card` and `Surface::field`, and the existing `combobox.h` styling (use the same combo class the other settings use).
- [x] **Step 4: Build; run the UI check;** expected no FAIL lines; `--theme-check` 0 failures.
- [x] **Step 5: Record progress.**

---

### Task 6: Meter in the chat panel (display only)

**Files:**
- Modify: `app/src/panels/chatpanel.h`, `chatpanel.cpp`, `app/src/mainwindow.cpp`
- Modify: `app/src/testing/uicheck.cpp` (fixture route `/inference/context-usage`)

**Interfaces:**
- Consumes: `ContextMeter` (Task 4), settings keys (Task 5), `POST /inference/context-usage` (Task 2).
- Produces on `ChatPanel`:
  - `public slots: void reloadContextSettings();` — re-reads both QSettings keys, applies the style, stores `m_autoCompactPercent`.
  - private `void refreshUsage();` — builds the body from `effectiveMessages()` plus the current typed text (as a pending user message when non-empty) and `max_tokens`; stale replies ignored with a `quint64 m_usageRequest` counter; 409/errors call `m_meter->clearUsage()`.
  - private `QJsonArray effectiveMessages() const;` — `[summary system message if m_compaction.covered>0] + m_history[m_compaction.covered:]`. (Used here and by Task 7's send flow.)
  - private `struct Compaction { QString summary; int covered = 0; } m_compaction;`
  - `m_meter` (`ContextMeter *`) placed beside `m_sendButton` in the `actions` row; the banner widget sits above the composer; Bar sits under it. `m_usageTimer` (single-shot, 400 ms) restarts on `m_input::textChanged`.
- Refresh triggers: after each completed/stopped reply, after `loadModel` success, `refreshEngineStatus`, `m_maxTokensSpin` change, `onClearChat`, `reloadContextSettings`, and the typing debounce.
- `MainWindow`: `connect(m_settingsPanel, &SettingsPanel::settingsChanged, m_chatPanel, &ChatPanel::reloadContextSettings);` next to the existing connections (line ~276).
- Warning text: from 70% the existing `setStatus` line shows `m_meter->warningText()` (tone `warning`) when no generation/error status is current; it must not overwrite an error status.

- [x] **Step 1: Write failing checks** in `uicheck.cpp`. Extend `Fixture` with `int usageUsed = 1000, usageLimit = 4096; int usageStatus = 200; QStringList usageRequests;` and a route `/inference/context-usage` returning `{"used":..,"reserved":<from request max_tokens>,"limit":..,"fill":..,"exact":true}` (read `max_tokens` from the request body) or status 409 when `usageStatus == 409`. Checks (using the existing event-loop wait helpers the file uses for the model/search sections):
  - Default settings: the chat panel contains a visible `contextRing`; Bar and banner hidden.
  - After the fixture returns used=1000, reserved=256 (`fill≈0.31`), tooltip text contains `"1,000 of 4,096"`.
  - Used=3000 → status label contains the warning (amber state), `level()` Warn; used=3900 → Critical.
  - Typing text triggers exactly one request after the debounce (`usageRequests.size()` grows by 1 after typing three characters quickly) and the typed text is in that request's last message.
  - Stale response ignored: two quick requests, the first answered late, final displayed value matches the second (use `holdUsage` like `holdChat`).
  - `usageStatus=409` → `m_meter` shows nothing (`warningText().isEmpty()`, ring tooltip empty), Send is still enabled, no error toast.
  - Changing `QSettings chat/meterStyle` to `bar` and calling `reloadContextSettings()` swaps visibility; `off` hides everything including the banner.
  - Response-tokens spin set to 8192 on `limit=2048` → warning text `"Reduce Response tokens"`.
  - Clear chat → meter requests usage for an empty history.
- [x] **Step 2: Build and run;** expected FAIL.
- [x] **Step 3: Implement** the pieces in Interfaces. Reuse the existing `QTimer` / request-id idioms in `chatpanel.cpp`.
- [x] **Step 4: Verify:** build clean, UI check no FAIL, `--theme-check` 0 failures, `--screenshot <scratch dir>` and open the PNGs for Ring/Bar/Ring+banner at Ok/Warn/Critical in dark and light themes (toggle via the existing theme switch).
- [x] **Step 5: Record progress.**

---

### Task 7: Auto-compact flow

**Files:**
- Modify: `app/src/panels/chatpanel.h`, `chatpanel.cpp`
- Modify: `app/src/testing/uicheck.cpp` (fixture route `/inference/compact`)

**Interfaces:**
- Consumes: `shouldCompact` (Task 4), `effectiveMessages`, `m_compaction`, `refreshUsage` (Task 6), `POST /inference/compact` (Task 3).
- Produces on `ChatPanel`:
  - private `void startSendFlow(const QString &text);` — `onSend` now validates, then calls this. It adds the user bubble and history entry exactly as `onSend` does today *after* the optional compaction step (so Stop during compaction leaves history untouched).
  - private `void compactThenSend(const QString &text);` — disables Send/Load/Clear, status "Compacting…", POSTs `{messages: m_history[covered:] as JSON, previous_summary, max_tokens, target_fill: autoCompact/100.0, keep_recent: 4}`. On 200: `m_compaction.covered += result.covered`; if `method=="summary"` set `m_compaction.summary = result.summary`; add the divider widget (objectName `contextDivider`, text `"Context compacted — earlier messages were summarized"` or `"Context compacted — earlier messages were dropped to make room"`) at the end of the transcript, then continue to `startSendFlow`. On any non-200/transport error: status warning "Could not compact; sending anyway.", continue to `startSendFlow` (the backend's own size error then applies if it truly can't fit).
  - `onStop` during compaction: `m_client->abortAll()`-style cancel via a request-id bump, restore the typed text into `m_input`, re-enable controls, status "Compaction stopped."
  - Trigger rule in `onSend`: compute `fill` from the latest known usage plus a fresh `/inference/context-usage` call including the new message; compaction runs only if `shouldCompact(fill, m_autoCompactPercent)` AND history has more than `keep_recent` messages AND `reserved/limit < autoCompact/100` AND compaction has not already run for this Send. Otherwise skip silently, showing the Task 6 warning text.
  - `onClearChat` resets `m_compaction`.
  - With meter style Off and compaction running, status shows "Compacting…" (no banner/ring).
  - Request JSON for `/inference/chat` uses `effectiveMessages()`.

- [x] **Step 1: Write failing checks** in `uicheck.cpp`. Extend `Fixture` with `compactStatus=200`, `compactMethod="summary"`, `compactCovered=4`, `holdCompact`, and `QStringList compactRequests`; route returns `{"summary":"The user asked about X.","covered":<compactCovered>,"method":"<compactMethod>"}`. Seed the chat panel with 8 history messages (add a small test-only accessor `seedHistoryForTest(const QList<QJsonObject>&)` guarded the same way as other test hooks in this file, or drive via the fixture's chat route 4 times). Checks:
  - Below threshold (fixture fill 0.60, setting 85): Send makes **no** compact request; one `/inference/chat` request.
  - At threshold (fill 0.85, setting 85): exactly one compact request, then the chat request; the chat request's first message has role `system` and starts with `"Summary of the earlier conversation:"`; its remaining messages are exactly `m_history[4:]` plus the new user message; `contextDivider` exists with the "summarized" text; the old bubbles are still in the transcript (bubble count unchanged + 2 new + divider).
  - Compact request body contains `keep_recent == 4`, `target_fill == 0.85`, `previous_summary == ""`.
  - Second compaction: `previous_summary` equals the first summary and the posted messages are only the tail; `m_compaction.covered` is the sum.
  - `method=="dropped"` → divider text says "dropped", `m_compaction.summary` unchanged (empty), covered advanced, message still sent.
  - `compactStatus=500` → warning status "Could not compact; sending anyway.", chat request still made, no divider.
  - Auto-compact Off (`0`) at fill 0.95 → no compact request.
  - History of 3 messages at fill 0.95 → no compact request, message still sent (Review Focus 1).
  - `Response tokens` ≥ limit → no compact request (Review Focus 2).
  - Stop during `holdCompact`: input text restored, history and `m_compaction.covered` unchanged, Send enabled, status "Compaction stopped." (Review Focus 3); the late reply is ignored.
  - Meter style Off + compaction → status "Compacting…" shown, no ring/banner visible.
  - Clear chat resets: next send has no leading system summary message.
  - Model unloaded mid-chat (usage 409) then reload: summary still sent (Review Focus 5).
- [x] **Step 2: Build and run;** expected FAIL.
- [x] **Step 3: Implement** per Interfaces; keep `m_history` append-only.
- [x] **Step 4: Verify** build clean, UI check no FAIL, `--theme-check` 0 failures; run full backend suite (196 + new) OK.
- [x] **Step 5: Record progress.**

---

### Task 8: Real-model end-to-end verification and docs

**Files:**
- Modify: `docs/development/architecture.md` (short "Context meter and compaction" section), `README.md` only if it lists chat features.
- No new product code.

- [x] **Step 1: Full automated verification:** `PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests -v` (all OK, count = 196 + new); `cmake --build build -j` (no warnings in new files); `./build/magnite-app --theme-check` ("0 failure(s)"); `./build/magnite-app --screenshot <scratch>` (inspect Ring/Bar/Ring+banner at Ok/Warn/Critical, dark and light).
- [x] **Step 2: Real small-model run** (`MAGNITE_SELFTEST_INFERENCE=1 ./build/magnite-app --selftest` still prints `[selftest] PASS (0 failures)`; then launch `./build/magnite-app`, load a small GGUF/toy model, set Response tokens to 256, chat until the meter passes 70% (amber + warning) and again until the chosen auto-compact percentage; observe: "Compacting…", divider, old bubbles still visible, next reply coherent; set the style to each of Bar / Ring + banner / Off and confirm; set auto-compact Off and confirm no compaction and that the backend's existing size error appears at 100%.) Record observations in the final report; do not claim success without having run these.
- [x] **Step 3: Docs:** add the architecture section (routes, `m_compaction`, settings keys).
- [x] **Step 4: Record progress** and report results with actual command output.

---

## Self-Review

- **Spec coverage:** meter styles + Off (T4, T5, T6); colours/levels (T4); warning in all styles except Off (T4 `warningText`, T6 status line, banner); settings 70–90 step 5, default 85, Off (T4 parse, T5); compact on Send ≥ threshold (T7); summary by loaded model, keep_recent 4 (T3); fallback dropping (T3); transcript untouched + divider (T7); repeat compaction via `previous_summary` (T3, T7); can't-help rules (T7 trigger rule, T6 warning text); no lock contention (T2 test); estimate `~` (T4 `usageText`, T2 `exact`); theme rules (T4); persistence-ready `{summary, covered}` (T6 struct); real-model check (T8).
- **Type consistency:** `split_for_compaction → int`, `drop_to_fit → int`, `CompactResult.covered` is relative to posted tail (T3) and added to `m_compaction.covered` (T7); `ContextMeterLogic` names identical across T4–T7; keys `chat/meterStyle`, `chat/autoCompactPercent` identical in T4, T5, T6.
- **Known judgement left to the implementer:** summarization prompt wording, debounce (400 ms suggested), pixel sizes, tooltip copy, where in `uicheck.cpp` the new blocks sit, whether `seedHistoryForTest` or fixture-driven turns seed history.
