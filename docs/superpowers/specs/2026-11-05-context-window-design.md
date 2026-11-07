# Context window meter and auto-compact — design

Date: 2026-11-05 · Feature 1 of 5 (context window, prompt editing, chat history, sidebar collapse, training charts). Each is designed and built on its own.

## Intent

Every model can only hold a limited amount of text at once (its *context window*). Today the chat grows silently and, once the conversation plus the reserved reply no longer fits, the backend rejects the request with an error (`_with_rag_context` in `backend/src/backend/routes/inference.py`). The user has no way to see how close they are and no way out except clearing the chat.

This feature (a) shows how full the context is, in the style Claude uses, (b) warns before it is full, and (c) if the user keeps going, compacts the older conversation into a summary so the chat can continue.

## Goals

1. A context meter in the chat panel showing tokens used out of the loaded model's limit. The limit is exact for llama.cpp and torch models. The token count is exact on torch and a close estimate on llama.cpp (chat-template overhead is approximated). Ollama and vLLM use the 2048 default. Estimated figures are marked with `~`.
2. Three selectable meter styles plus Off, chosen in Settings:
   - **Ring** — small ring beside Send; hover tooltip gives exact numbers.
   - **Bar** — thin bar under the typing box with always-visible numbers, e.g. `1,390 / 4,096 tokens`.
   - **Ring + banner** — Ring, plus a banner above the typing box when the context is nearly full.
3. Colour by fill: green below 70%, amber 70–90%, red above 90%.
4. A warning from 70% upward, shown in **every** style except Off (see Open calls for Off). In Ring and Bar the warning is the amber/red colour plus a one-line message in the existing status line under the typing box. In Ring + banner it is also the larger banner above the typing box. Above 90% the message says the chat is nearly full and auto-compact will run (or, if auto-compact is Off, that the next message may fail).
5. Auto-compact, configurable in Settings as Off or a percentage from 70 to 90 in steps of 5 (default 85). It fires when the user presses Send and the fill is at or above that percentage.
6. Compaction: the loaded model summarizes the older messages; the most recent messages are kept verbatim; the summary replaces the older messages *in what is sent to the model*.
7. The on-screen transcript is untouched. A divider in the chat says the context was compacted.
8. If summarizing fails or the result still doesn't fit, fall back to dropping the oldest messages so the user's message still goes through.

## Non-goals

- Persisting chats across restarts (feature 3, chat history). This design must not block it: compaction state is a small serializable struct (summary text + number of covered messages) so feature 3 can store it later.
- Editing the system prompt (feature 2).
- Counting RAG-retrieved chunks in the meter. The backend already trims RAG context to whatever room is left; the meter reflects the chat only.
- Changing a model's context size (the `n_ctx` the engine loads with).
- Exact token counts for every engine (see Constraints).

## Anti-goals (failure even if it "works")

- Compaction that happens without the user being told why. The divider always appears after it. The warning appears before it in every meter style except Off; with Off, a status message appears while it runs.
- Losing the user's visible conversation or their just-typed message.
- Blocking the UI thread, or leaving Send stuck disabled if compaction fails or is stopped.
- A meter that lies by a wide margin, or that flickers/lags while typing.

## Constraints and facts from the codebase

- Engines already expose `context_size()` and `count_messages()` (`engines/base.py`, `torch.py`, `llamacpp.py`). `count_tokens` is exact for torch and llama.cpp; the base-class fallback is a byte-count estimate (used by Ollama/vLLM). `count_messages` is exact on torch (counts the rendered prompt) and an estimate elsewhere (`128 + tokens + 32/message`). The meter therefore shows "~" when the figure is an estimate. The backend reports `exact: bool`.
- llama.cpp engine loads with `n_ctx=2048` by default, so small limits are the common case.
- The client owns the history (`ChatPanel::m_history`, a `QList<QJsonObject>`) and sends all of it on every `/inference/chat`. The backend is stateless.
- Settings are `QSettings` keys written from `SettingsPanel` (e.g. `appearance/studio3Ambient`).
- Theme rules (AGENTS.md): no hex values in widget code or QSS; add tokens to `app/src/theme.h` and extend `Theme::selfCheck()`.
- New C++ files go into `MAGNITE_SOURCES` in `app/CMakeLists.txt`.
- Per the user's standing instruction, changes stay local/uncommitted.

## Decisions made with the user

- Styles: Ring, Bar, Ring + banner, Off — selectable in Settings.
- Thresholds: amber from 70%, red from 90%. Auto-compact threshold is user-set between 70 and 90, with an Off choice.
- Warn first; compaction only on Send at or above the threshold.
- Old messages remain visible after compaction, with a "context compacted" note.

## Design

### Definition of "fill"

`fill = (tokens(messages) + reserved_reply_tokens) / context_size`, where `reserved_reply_tokens` is the "Response tokens" value in the chat options (`max_tokens`). This is the same quantity the backend rejects on, so 100% means "the next request will fail". The tooltip breaks the numbers out: used, reserved for reply, limit.

### Backend (`backend/src/backend/inference/context.py`, new; routes in `routes/inference.py`)

- `POST /inference/context-usage` — body `{messages, max_tokens}` → `{used, reserved, limit, fill, exact}`. 409 if no model is loaded (a `RuntimeError` maps to 409, as in `/inference/load`); the client treats this as "no meter yet".
- `POST /inference/compact` — body `{messages, previous_summary (optional), max_tokens, target_fill, keep_recent (default 4)}` → `{summary, covered, method: "summary" | "dropped"}`. `target_fill` is the user's auto-compact percentage as a fraction. `covered` is the number of leading posted messages the summary stands in for.
  1. Pick the split: keep the last `keep_recent` messages, moving the split earlier if needed so the kept part starts at a user message (a user/assistant pair is never split). If there is nothing older to summarize, return `covered: 0`.
  2. Ask the loaded engine to summarize the older messages (and `previous_summary`, if any) with a fixed instruction (non-streaming, bounded `max_tokens`). Cancelling the request cancels generation (client disconnect returns 499, as the other inference routes do).
  3. Verify `summary + kept messages + reserved reply` is below `target_fill` of the window. If summarization raised, returned empty, or the result is still too large, return `method: "dropped"`: `summary` is `previous_summary` unchanged (empty if none) and `covered` is the number of oldest messages dropped so the remainder is below `target_fill` (or, if that is impossible, as small as the window allows).
- Pure functions (`split_for_compaction`, `drop_to_fit`, `fill_of`) separated from the engine call so they test with a fake engine.
- **Repeat compactions.** The client posts only the *uncompacted* tail (`m_history[covered:]`) plus the existing summary as `previous_summary`. The backend folds the old summary into the new one. The response's `covered` is relative to the posted tail; the client adds it to its running total. So `m_compaction.covered` is always an index into the full `m_history`, and feature 3 persists just `{summary, covered}`.
- **No lock contention.** `context-usage` only reads token counts; it must not take the supervisor's non-blocking `operation()` lock, so typing-time requests never fail with "busy" during generation or compaction. `compact` does take it, like chat.
- **When compaction can't help.** Compaction is skipped (no slow summarization) when: the kept recent messages plus reserved reply already exceed the auto-compact percentage, or `reserved` alone is at or above the window, or there is nothing older to summarize. It runs at most once per Send. In those cases the user sees a specific warning instead ("Reduce Response tokens" when the reserve is the cause; "Start a new chat" otherwise), and the existing backend error still applies if the request truly cannot fit.

### Client

- **State** in `ChatPanel`: `m_compaction { QString summary; int covered; }`. Messages sent = optional leading `system` message "Summary of the earlier conversation: …" + `m_history[covered:]`. `onClearChat` resets it. `m_history` itself is never trimmed, so the transcript stays.
- **`ContextMeter` widget** (new, `app/src/widgets/contextmeter.{h,cpp}`): one widget that renders Ring / Bar / Off and the banner, driven by `setUsage(used, reserved, limit, exact)`. Pure threshold/colour logic lives in free functions (`levelFor(fill)`, `shouldCompact(fill, setting)`) so they can be tested without a window.
- **Refresh triggers:** after each completed reply, on model load/unload, on Response-tokens change, on clear, and while typing (debounced ~400 ms, request only when a model is loaded; ignore stale responses with the same request-id pattern `onSend` uses).
- **Send flow:** `onSend` → compute fill from the last usage + the new message (ask the backend if needed) → if auto-compact is on and fill ≥ threshold: disable input, show "Compacting…", call `/inference/compact`, store `m_compaction`, add the divider ("Context compacted — earlier messages were summarized" or "…dropped" for the fallback), then continue the normal send. Stop cancels compaction and restores the input text.
- **Settings** (new "Chat" card in `SettingsPanel`): meter style combo; auto-compact combo/slider (Off, 70–90 step 5). Keys `chat/meterStyle` (`ring|bar|ringBanner|off`), `chat/autoCompactPercent` (0 = off). The panel re-reads them on change (same pattern as the appearance toggles).
- **Theme:** new tokens for meter track and level colours (reusing `statusSuccess/Warning/Error` where possible); no raw hex; extend `Theme::selfCheck()`.

## Testing

Backend (`backend/tests/test_context_window.py`, unittest, no model downloads — fake engine):
- `fill_of` arithmetic and edge cases: zero messages, exactly at limit, over limit, `max_tokens` larger than the window.
- `context-usage`: 409 with no model; correct numbers with a fake engine; `exact` flag.
- `split_for_compaction`: fewer messages than `keep_recent`; odd/even counts; leading system message; pairs not split.
- `compact`: happy path (summary used); engine raises → `dropped`; empty summary → `dropped`; summary too long → `dropped`; nothing to compact → `covered: 0`; cancellation; `previous_summary` is passed to the engine and kept when dropping; result lands below `target_fill`.
- Regression: existing `/inference/chat` behaviour unchanged (suite stays green; 196 backend tests at last count on 2026-11-02).

Client:
- Logic functions `levelFor` / `shouldCompact` and settings parsing (including invalid stored values → defaults) as `--selftest`/uicheck cases at the thresholds 69/70/89/90/100%.
- `--theme-check` stays at 0 failures with the new tokens.
- `--screenshot` of each style and each level for visual review in dark and light themes.
- Manual end-to-end on a real small model: fill a 2048-token chat, observe warning at 70%, compaction at the chosen percentage, divider, and that the next reply stays coherent.

## Decided after review

- Ollama and vLLM keep the hardcoded 2048 limit for now (they don't override `context_size()`). The meter labels it as estimated (`~`). Looking up their real context window is a separate follow-up, not part of this feature.

## Open calls (mine — flag if you disagree)

- Default meter style **Ring**; default auto-compact **85%**.
- Style **Off** hides the meter and the warning banner, but auto-compact still follows its own setting (if auto-compact is on, a one-line status message appears when it runs). If this is surprising, the alternative is to disable auto-compact when the meter is Off.
- `keep_recent` default 4 messages.
- Fill includes the reserved reply tokens.
- Estimates are shown with a "~" prefix.
- Summaries are generated by the loaded model itself; no separate model.

## Left to the builder

Exact summarization prompt wording; debounce interval; pixel sizes and tooltip copy within the existing theme; internal naming; where in `uicheck.cpp` the new cases are registered.
