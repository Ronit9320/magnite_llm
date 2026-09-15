# Phase 5.5 — Space Theme UI Redesign (Modular Build Plan)

Sub-phases for the deep-space visual redesign. Each sub-phase is self-contained, lands independently, and is tested/verified before the next one starts. `design_doc.md` is the single source of truth for all visual decisions — do not improvise outside it; flag anything it doesn't cover.

## Locked Decisions (resolved before work begins)

- **Glass approach:** Widgets-with-fake-glass (design doc §7 option 1). Semi-transparent fills + borders + glow, no real blur, no QML.
- **Models / Adapters / RAG:** convert `QTableWidget`s to glass card lists (design doc §8).
- **Chat:** prototype both variants (styled-text vs. real bubble widgets), compare live, keep the winner.
- **Icons:** bundle Lucide-style line SVGs (~18 icons), stroke-tinted per state.
- **Fonts:** bundle Space Grotesk + DM Sans (Google Fonts, SIL OFL) into `resources`, load via `QFontDatabase`; fall back to system fonts if files are missing.
- **Motion:** 150ms panel crossfade; hover/focus state changes via QSS. Nav-glow pulse (§6) is skipped unless a visual test asks for it.

---

## Phase 5.5.0: Theme Infrastructure

**Goal:** All design tokens (design doc §2–§3) live in exactly one place, and the app can consume them. Nothing visible changes yet.

- Create `app/src/theme.h` + `app/src/theme.cpp` — `Theme` singleton:
  - Static `QString` constants for every token: `bgVoid`, `bgDeep`, `accentPrimary`, `accentBlue`, `accentGlow`, `accentPink`, `glassFill`, `glassFillHover`, `glassFillActive`, `glassBorder`, `glassBorderHover`, `glassBorderActive`, `textPrimary`, `textSecondary`, `textMuted`, `statusSuccess`, `statusWarning`, `statusError`, `statusInfo`.
  - Typography/size constants (h1–h3, body, body-sm, mono; corner radii; sidebar widths; spacing base).
  - `static QString qss()` composing the global stylesheet from those constants.
  - Font-family helpers (`headingFont()`, `bodyFont()`, `monoFont()`).
- Create `app/resources/theme/theme.qss` as the structured master stylesheet (colors injected from `Theme`):
  - `QPushButton` primary / secondary / destructive (object-name selectors: `#primaryBtn`, `#secondaryBtn`, `#destructiveBtn`).
  - Inputs: `QLineEdit`, `QComboBox`, `QSpinBox`, `QDoubleSpinBox` — glass fill, glass border, muted placeholder, focus ring.
  - `QGroupBox` → glass card. `QSlider` → blue fill + glowing thumb.
  - Scrollbars: thin, dark. `QLabel` heading vs. body classes.
  - Status label styles keyed off `text-muted`.
- Download fonts into `app/resources/fonts/` (Space Grotesk + DM Sans variable/weight TTFs) + `OFL.txt`.
- Download Lucide line SVGs into `app/resources/icons/` for: models, training, rag, adapters, chat, settings, search, refresh, download, trash, plus, export, import, send, stop, clear, load.
- Create `app/resources.qrc` bundling `theme/`, `fonts/`, `icons/`.
- Update `app/CMakeLists.txt`: add `resources.qrc` to sources.
- Add a tiny `Theme::coloredIcon(name, color)` helper that loads an SVG and substitutes the stroke color (used by all later phases).

**Test (this phase):**
1. `cmake --build build` compiles cleanly (AUTORCC picks up the qrc).
2. `Theme` constants print correct hex values in a quick debug check (`./magnite-app` with a temporary debug assertion or a unit print via env flag) — or simply a `static_assert`-style sanity check that `Theme::bgVoid() == "#000000"` etc. in a debug build.
3. Fonts + qrc embed: `ls` inside the built binary's resource tree via `QT_PLUGIN_PATH` is overkill — instead, assert `QFontDatabase::families()` contains the loaded families after `addApplicationFont` in a debug run.

No visual changes expected. If the app renders identically, the phase passed.

---

## Phase 5.5.1: App Shell — Background, Fonts, Sidebar

**Goal:** The main window becomes visibly space-themed: flat black background, custom typefaces active, glass sidebar with icons and active-state glow bar. Panels still look default inside.

- `app/src/main.cpp`:
  - Load bundled fonts via `QFontDatabase::addApplicationFont` (guard with fallback).
  - Apply `Theme::qss()` via `qApp->setStyleSheet()`.
  - Set default app font to DM Sans.
- `app/src/mainwindow.cpp`:
  - Central widget: flat black background (design doc §2 `bg-void`) — no per-frame repaint cost (static).
  - Sidebar: 240px glass fill, logo in Space Grotesk, 5 nav items (Models, Training, RAG, Adapters, Chat) each with a Lucide icon, Settings pinned at the bottom with a stretch spacer.
  - Active nav item: blue icon + text + 2px glowing left-edge bar; hover: `glass-fill-hover`; inactive: `text-secondary`.
  - Backend status label → themed pill (`status-success` green when connected, `status-error` on failure, muted while starting).
  - Enforce minimum window size 1024×680.
- Panel switching: wrap the destination panel in a `QGraphicsOpacityEffect` and animate opacity 0→1 over 150ms (crossfade, design doc §6). No slide/bounce.

**Test (this phase):**
1. Rebuild; launch `./magnite-app`.
2. Visual walk-through: sidebar renders with icons, active state, hover states; background is static (no CPU burn — check `top` stays flat while idle).
3. Click each nav item: panel crossfades in 150ms; the sidebar active indicator tracks the click; Settings sits pinned at the bottom.
4. Backend connects: status pill flips to success green. Kill backend process (simulate) or disconnect to check error state if feasible.
5. `./magnite-app --selftest` — the "navigated through all panels" check must still pass (nav logic unchanged, only visuals).
6. Resize window: cannot shrink below 1024×680.

---

## Phase 5.5.2: BasePanel Header + Shared Component Restyle

**Goal:** Every panel gets the themed header (Space Grotesk title, secondary subtitle, 32px padding) and the shared controls (buttons, inputs, group boxes, scrollbars, status labels) stop using inline hex colors and pick up the theme.

- `app/src/panels/basepanel.cpp`:
  - Panel title → Space Grotesk, `h1` size, `text-primary`; subtitle → DM Sans `body-sm`, `text-secondary`.
  - Root layout padding → 32px (design doc §4).
  - Object names so global QSS targets them.
- Across all panels (`modelspanel.cpp`, `trainingpanel.cpp`, `adapterspanel.cpp`, `chatpanel.cpp`, `ragpanel.cpp`, `settingspanel.cpp`):
  - Strip inline `setStyleSheet` calls for status colors; replace `"#666"`, `"#c0392b"`, `"#27ae60"`, `"#888"` etc. with `Theme::muted()`, `Theme::error()`, `Theme::success()`.
  - Tag buttons with object names (`#primaryBtn` / `#secondaryBtn` / `#destructiveBtn`) per their role in that panel.
  - Let global QSS own button/input/group-box/scrollbar appearance — remove redundant per-widget styling.
  - Training log + Models/Metrics monospace text → `mono` font family + `text-secondary`.
- Sliders (any present: chat max-tokens/temp use spin boxes today, no sliders yet — if any exist, style per §5: blue filled track, glowing thumb).

**Test (this phase):**
1. Rebuild + run; visit every panel (Training, Settings especially — the form-heavy ones).
2. Visual check per §5: primary buttons are solid blue with glow, secondary are glass, destructive are `status-error`; disabled buttons are muted/half-opacity with no glow; focus ring appears on inputs.
3. `QGroupBox` sections in Training/Settings/RAG render as glass cards with 12px radius and `glass-border`.
4. Status labels inherit `text-muted` by default and only show color on error/success messages.
5. `./magnite-app --selftest` — settings round-trip for Models/Training/RAG/Chat must still pass (persistence keys unchanged).
6. Contrast spot-check: `text-secondary` (#A1A1AA) on `bg-deep`/`bg-void` at 12px must be readable (design doc §7). Flag if not.

---

## Phase 5.5.3: Models Panel — Glass Card Grid

**Goal:** The Models `QTableWidget` is replaced by a paginated/scrollable grid of glass model cards (design doc §8) with full feature parity: browse local, search Hub, download with live progress, delete, selection state.

- New widget `app/src/panels/ModelCard` (header + cpp, or a small `cards/` subfolder if it grows): glass card containing name (Space Grotesk), source (HF / Ollama) with icon, architecture **status pill** (`status-info`), size, license; interactive hover → `glass-border-active` + `accent-glow` shadow.
- `modelspanel.cpp`:
  - Swap `QTableWidget` for `QScrollArea` + `FlowLayout` (add a compact `FlowLayout` helper, ~60 lines) or a single-column list (`QListWidget` with item widgets) if the grid feels cramped in testing.
  - Local view: cards for registered models + a thin gradient progress bar (`accent-blue → accent-primary`) on the currently-pulling card with `%` in mono.
  - Hub view: search-result cards (name, downloads, likes, license) — same card component with different badges.
  - Preserve: `refresh()`, `onSearch`/returnPressed, view combo (Local / Hub), download/delete buttons acting on the selected card, `setStatus`, settings persistence (`lastSearch`), `modelsChanged` signal, pull polling (`kPullPollMs`).
  - Selection = click card highlight (`glass-border-active`); keyboard navigation optional.

**Test (this phase):**
1. Rebuild + run → Models panel.
2. With the backend up: local models render as cards; cards hover correctly; architecture badge shows.
3. Search the Hub (e.g. "gpt2"): results render as cards; select one → Download works; the selected card shows live progress `%`; on completion the Local view refreshes with the card present.
4. Select a local model → Delete works and the card disappears.
5. Hidden regression: `MAGNITE_SELFTEST_PULL=1 ./magnite-app --selftest` passes (pull → poll → registered e2e), which proves `onDownload`/polling still work without the table.
6. `./magnite-app --selftest` — settings round-trip for Models still passes (`lastSearch` preserved).

---

## Phase 5.5.4: Adapters Panel — Glass Card List

**Goal:** Adapters `QTableWidget` → glass card list (design doc §8) with full parity: activate/deactivate toggle, export/import, delete, live Active state.

- New `AdapterCard`: name (Space Grotesk), base-model badge, rank, method pill (`LoRA`/`QLoRA`), size, trained date; **Active card has `glass-border-active` + glow** per design doc §8; card-level **Activate primary button** (re-click on the active card clears the adapter, matching current toggle behavior); secondary Export/Import/Delete buttons in a card footer.
- `adapterspanel.cpp`:
  - Replace table with the card list.
  - Preserve: `refresh()` (loads `/adapters` + `/inference/status`), `loadActiveAdapter`, `onActivate` toggle semantics, `onExportAdapter`, `onImportAdapter` (file dialog), `onDeleteAdapter`, `adapterActivated` signal, status messages.
  - Empty state: a friendly centered hint ("No adapters yet. Train one in the Training panel.").

**Test (this phase):**
1. Rebuild + run → Adapters panel.
2. If a trained adapter exists, confirm its card renders with badges/date; activating it (or a fresh one) highlights the card with the glow border and the Chat status line later shows `| Adapter:`.
3. Re-click Activate on the active card → clears + removes glow (toggle semantics intact).
4. Export a card → success status; import the exported zip → new card appears (409 on duplicate preserved).
5. Delete the active card → clears first, card list refreshes.
6. `MAGNITE_SELFTEST_ADAPTER=1 ./magnite-app --selftest` passes (trains tiny adapter → activates → streams via card UI path — actually via API; the key is the panel logic around active-id still syncs).
7. Settings round-trip + panel count checks still green.

---

## Phase 5.5.5: RAG Panel — Document Cards + Config Glass Card

**Goal:** RAG placeholder panel styled: document list as glass cards (one per file, type icon), retrieval config as a glass card §8. Functionally still a Phase-6 placeholder.

- `ragpanel.cpp`:
  - Replace `QListWidget` with a card list; each `RagDocCard`: file name, type icon (pdf/txt/md/html), path. Removable via Remove Selected; the cards are cosmetic until Phase 6 stores them.
  - Retrieval config (`QGroupBox`, chunk size / overlap / top-K) → glass card per design doc §4/§8.
  - Buttons (Add Documents / Remove / Clear) → themed (primary/secondary).
  - Status label → `text-muted`.
- Keep `settingsJson`/`applySettingsJson` (chunkSize, chunkOverlap, topK) untouched.

**Test (this phase):**
1. Rebuild + run → RAG panel.
2. Add Documents: cards appear with correct type icons; Remove Selected and Clear All update the list.
3. Config card renders as a glass card; spin-box values persist across restart (settings round-trip).
4. `./magnite-app --selftest` — RAG settings round-trip passes.

---

## Phase 5.5.6: Chat Panel — Bubbles (Both Variants, Compare, Keep Winner)

**Goal:** The conversation region becomes per-design-doc (§5): user messages right-aligned, model messages left-aligned with a 2px `accent-primary` left border; streaming shows a blinking block cursor. Implement both variants, run, compare, keep one.

- **Variant A — Styled text:** keep a single read-only region; render appended messages as HTML blocks (user: right, `text-primary` on `glass-fill-hover`; model: left, `glass-fill` + blue left border). Streaming block appended at the end, cursor = `▍`.
- **Variant B — Real bubbles:** `QScrollArea` (auto-scroll to bottom) holding `ChatBubble` widgets; user bubble `glass-fill-hover` right-aligned `text-primary`; model bubble `glass-fill` left-aligned + 2px blue left border; streaming bubble shows a `▍` block toggled by a 500ms `QTimer` while `m_streaming`.
- Either way, do **not** touch the streaming plumbing: `onSend`/`streamPost` event handling, `onStop`/`abortStream`, history append, `m_pendingResponse`. Only the presentation layer changes.
- Top of panel: pinned model/engine/adapter indicator (restyle existing status line into a pill row); RAG toggle stays near the input box (§8).
- Generator controls row (model combo, engine combo, Load Model, tokens/temp, Refresh Status) inherits Phase 5.5.2 component styles.
- Compare target: launch with a tiny backend model (`MAGNITE_SELFTEST_INFERENCE=1` or manual `./magnite-app` + `sshleifer/tiny-gpt2` / a GGUF) and send 2–3 messages, watching streaming.

**Test (this phase):**
1. Rebuild + run → Chat panel.
2. Manual: load a model, send "Hello", watch streaming cursor tick and stop; send a follow-up so a user+model pair shows alignment. Cut with Stop mid-generation → message retained, cursor gone.
3. Compare **Variant A vs B** side by side (build flag, e.g. `#define CHAT_VARIANT` or run one at a time). Check: readability, alignment clarity, streaming feel, scroll behavior during long streams. **Pick the winner and delete the loser** before moving on.
4. Clear Chat works; settings round-trip (engine/tokens/temp/rag/model) passes.
5. `MAGNITE_SELFTEST_INFERENCE=1 ./magnite-app --selftest` passes (streamPost e2e untouched).

---

## Phase 5.5.7: Motion Pass, Contrast, Final Verification

**Goal:** Polish per design doc §6 and §7, then a full pass to confirm the theme is centralized and no regression slipped in.

- **Motion check (done in earlier phases, revisit):** panel crossfade 150ms (5.5.1); hover/focus transitions are QSS pseudo-state based (§6 — no timers needed); confirm no animated background anywhere; confirm the nav-glow pulse is absent and decide if it should be added for testing.
- **Contrast pass (design doc §7):** walk every panel at 12px `body-sm` — `text-secondary` on `bg-void`/`bg-deep`; check progress % text, badges, subtitles, status labels. Fix by bumping to `text-secondary`-on-`bg-deep` or using `text-primary` where needed — record any deviation in design_doc.md as a flagged amendment.
- **Centralization audit:** `grep` the `app/src` tree for hex color literals (`#[0-9a-fA-F]{3,8}`) and inline `setStyleSheet` — remaining hits should only be inside `theme.cpp` (and pre-existing non-visual uses). Move any stragglers into `Theme`.
- **Icon audit:** every button/nav item shows an icon in the correct state color (blue active / secondary idle); no blank-icon glitches on HiDPI (check at 1x and 2x devicePixelRatio if available).

**Test (this phase):**
1. Full clean rebuild: `cmake -S app -B build && cmake --build build`.
2. Launch and screenshot each panel under identical window size — review as a set for consistency (unified spacing, radius, accent usage, `accent-pink` used at most once).
3. Run the bare selftest: `./magnite-app --selftest` → PASS (0 failures).
4. Run phase selftests that exercise touched panels:
   - `MAGNITE_SELFTEST_PULL=1 ./magnite-app --selftest`
   - `MAGNITE_SELFTEST_INFERENCE=1 ./magnite-app --selftest`
   - `MAGNITE_SELFTEST_TRAINING=1 ./magnite-app --selftest`
   - `MAGNITE_SELFTEST_ADAPTER=1 ./magnite-app --selftest`
5. Idle CPU check: flat background + crossfade effects consume ~0% when idle (no repaint loops).
6. Update `BUILD_PLAN.md`: mark Phase 5.5 done with a delivery note referencing this file and any design-doc deviations.

---

## Work Order & Dependencies

```
5.5.0 Theme Infrastructure
  └─> 5.5.1 App Shell (fonts/bg/sidebar/crossfade)
        └─> 5.5.2 Shared Component Restyle
              ├─> 5.5.3 Models Cards
              ├─> 5.5.4 Adapters Cards
              ├─> 5.5.5 RAG Cards
              └─> 5.5.6 Chat Bubbles
                    └─> 5.5.7 Motion, Contrast, Final Audit
```

- 5.5.2–5.5.6 depend on 5.5.0–5.5.2 being green.
- 5.5.3 and 5.5.4 are independent of each other; 5.5.6 is independent of 5.5.3–5.5.5 but its status row consumes the adapter indicator semantics from 5.5.4.
- No sub-phase ships ahead of its own test block passing.