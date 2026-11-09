# Collapsible Sidebar (feature 4/5) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (the user chose in-session native execution) to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the left menu fold into a slim icon strip (like Claude's website) and open again, remembered across restarts, with New chat still one click away.

**Architecture:** The sidebar, today built inline in `MainWindow::setupUi()`, becomes its own `Sidebar` widget so `--ui-check` can build it without a backend. A plain `SidebarState` class holds every decision (collapsed or not, widths, what is visible, tooltip text, the stored setting). `Sidebar` applies it: the same nav buttons switch between icon+text and icon only, a toggle button and Ctrl+B flip the state, a small + button appears under Playground while collapsed.

**Tech Stack:** C++20 / Qt6 Widgets, QSettings, QPropertyAnimation/QVariantAnimation, the `check(...)` harness in `app/src/testing/uicheck.cpp` (`./build/magnite-app --ui-check`) and the base `--selftest` in `MainWindow::runSelfTest`.

**Spec:** `docs/superpowers/specs/2026-11-07-collapsible-sidebar-design.md`

## Global Constraints

- No hex colours in widget code or QSS (AGENTS.md). Existing tokens (`@insetBackground`, `@surfaceFill`, `@surfaceFillHover`, `@surfaceBorder`, `@textPrimary`, `@textSecondary`, `@accentPrimary`, `@accentLabel`) should be enough; if a new colour is needed add a token in `app/src/theme.h`, `tokens()` in `theme.cpp`, and extend `Theme::selfCheck()`. Icons: reuse `chevron-left`, `chevron-right`, `plus` (already bundled), tinted with `Theme::coloredIcon()`.
- New C++ files go in `MAGNITE_SOURCES` in `app/CMakeLists.txt` (explicit list).
- Widths: expanded **176**, collapsed **52** (a starting guess; adjust a few pixels only if icons/marker/focus ring do not fit, and then change it in `SidebarState` only).
- Setting key `appearance/sidebarCollapsed` (bool). Reduced motion key `appearance/studio3ReducedMotion`. Slide time about 180 ms; snaps in `--ui-check`, `--selftest`, `--screenshot` runs and with reduced motion.
- Toggle shortcut **Ctrl+B**, window-wide, works while the chat box has focus. Ctrl+1 to Ctrl+6 keep working in both states. Tooltips stay exactly as today ("Library · Ctrl+1" … "Settings · Ctrl+6"); toggle tooltips "Collapse sidebar · Ctrl+B" / "Expand sidebar · Ctrl+B"; + button tooltip and accessible name "New chat".
- The connection indicator and the light/dark toggle live in the window's top bar; they do not move and the strip has no connection dot.
- `ChatHistoryList` itself does not change. No backend change (suite stays 228 tests OK: `PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests`). No `DATA_SCHEMA` change.
- `Theme::minWindowHeight()` stays 680; the expanded sidebar (including the chat list shrinking to two rows) must fit at 176x680 exactly as today, and the strip must fit at 52x680.
- Checks must not touch the real `QSettings` (`--ui-check` redirects it to a temp folder; remove keys you set at the end of each check block).
- Local only: **no commits and no push until the last step**; the single commit is `feat(ui): feature 4/5 - collapsible sidebar` with **no Co-Authored-By line and no Claude footer** (standing user instruction, overrides any default attribution). Include the spec and this plan in that commit. Every task ends with "Record progress": tick that task's checkboxes in this file.
- Baseline: `./build/magnite-app --ui-check` = about 586–588 ok with only the known "training/settings dropdown supports keyboard selection" failures (3 or 4 in total); the first run after a rebuild may show 1–3 extra timing failures, re-run before investigating. `--theme-check` = 0 failures. Build: `cmake --build build -j`. After restoring files from a backup, `touch` the sources first.

## Review Focus

1. Focus left on a hidden widget: the chat list had keyboard focus when the sidebar collapsed; the next Tab must not go nowhere. (Task 4)
2. The + and the list's "New chat" row disagree while a reply streams / a model loads / an editor is open. (Task 4)
3. Theme switch (light/dark) while collapsed: stale icon tint or missing orange marker. (Task 3)
4. Rapid double toggle (button then Ctrl+B within the slide): must end in the last requested state with exact widths 176/52. (Task 4)
5. Stored value that is missing, garbage or a string ("true"): start expanded for missing/garbage, collapsed for "true"; start-up shows the saved state at once with no flash. (Tasks 1, 3)

---

## File Structure

| File | Responsibility |
|---|---|
| `app/src/widgets/sidebarstate.{h,cpp}` (new) | `SidebarState`: pure decisions and the stored setting. No widgets. |
| `app/src/widgets/sidebar.{h,cpp}` (new) | `Sidebar` widget: brand block, toggle, nav buttons, chat list slot, + button, Settings; applies `SidebarState`; slide animation; shortcuts. |
| `app/src/mainwindow.{h,cpp}` (modify) | Creates the `Sidebar`, wires panel switching, chat list, busy state, new chat, appearance, screenshot env var; removes the inline sidebar code and `m_sideButtons`. |
| `app/resources/theme/console.qss` (modify) | Collapsed variation (`QWidget#sidebar[collapsed="true"]`), toggle button, + button. Tokens only. |
| `app/src/testing/uicheck.cpp` (modify) | All new checks. |
| `app/CMakeLists.txt` (modify) | Add the new sources. |
| `docs/development/architecture.md` (modify) | Short "Collapsible sidebar" note after "Chat history". |

---

### Task 1: `SidebarState` (the decisions, tested on their own)

**Files:**
- Create: `app/src/widgets/sidebarstate.h`, `app/src/widgets/sidebarstate.cpp`
- Modify: `app/CMakeLists.txt` (add both to `MAGNITE_SOURCES`), `app/src/testing/uicheck.cpp` (include header; new block after the Greeting checks near line 1974)

**Interfaces:**
- Produces:
```cpp
class SidebarState {
public:
    static constexpr int kExpandedWidth = 176, kCollapsedWidth = 52;
    static constexpr const char *kSettingsKey = "appearance/sidebarCollapsed";
    explicit SidebarState(bool collapsed = false);
    static SidebarState load();            // missing or unreadable value -> expanded
    void save() const;
    bool collapsed() const;
    void setCollapsed(bool collapsed);
    bool toggle();                         // returns the new collapsed value
    int width() const;                     // 176 or 52
    bool showLabels() const;               // !collapsed (wordmark, tags, nav text)
    bool showHistory(bool playgroundOpen) const;      // !collapsed && playgroundOpen
    bool showNewChatButton(bool playgroundOpen) const; // collapsed && playgroundOpen
    QString toggleTooltip() const;         // "Collapse sidebar · Ctrl+B" / "Expand sidebar · Ctrl+B"
};
```

- [x] **Step 1: Write the failing checks** (new block, before any implementation; `#include "widgets/sidebarstate.h"`)
  - `check` names/assertions: default `SidebarState` → `!collapsed() && width()==176 && showLabels() && showHistory(true) && !showHistory(false) && !showNewChatButton(true)`, message "sidebar state: expanded by default".
  - after `setCollapsed(true)` → `width()==52 && !showLabels() && !showHistory(true) && showNewChatButton(true) && !showNewChatButton(false)`, "sidebar state: collapsed hides labels and list, offers + only on Playground".
  - `toggleTooltip()` is "Collapse sidebar · Ctrl+B" expanded and "Expand sidebar · Ctrl+B" collapsed; `toggle()` returns the new value and flips twice back.
  - Persistence: after `QSettings().remove(key)`, `!SidebarState::load().collapsed()`; `SidebarState(true).save()` then `load().collapsed()` and `QSettings().value(key).toBool()`; `SidebarState(false).save()` then `!load().collapsed()`; `QSettings().setValue(key,"true")` loads collapsed; `QSettings().setValue(key,"banana")` loads expanded; finally `QSettings().remove(key)`.
- [x] **Step 2: Run to verify failure**
  Run: `cmake --build build -j 2>&1 | tail -5` — Expected: compile error, `sidebarstate.h` not found.
- [x] **Step 3: Implement `SidebarState`** in the two new files exactly per the interface; `load()` reads `QSettings().value(kSettingsKey, false).toBool()`.
- [x] **Step 4: Run to verify pass**
  Run: `cmake --build build -j && ./build/magnite-app --ui-check 2>&1 | grep "sidebar state"` — Expected: every line `ok:`; then the failure count equals the baseline.
- [x] **Step 5: Record progress** (tick this task's boxes).

---

### Task 2: Extract the sidebar into `Sidebar` (expanded behaviour unchanged)

**Files:**
- Create: `app/src/widgets/sidebar.h`, `app/src/widgets/sidebar.cpp`
- Modify: `app/CMakeLists.txt`, `app/src/mainwindow.{h,cpp}` (setupUi lines ~160–277 replaced; `switchPanel`, `refreshNavIcons`, `m_sideButtons`, `m_history`, `applyAppearance`'s `brandSeal`), `app/src/testing/uicheck.cpp`

**Interfaces:**
- Consumes: `SidebarState` (Task 1), `ChatHistoryList`, `StudioCanvas`, `BrandWordmark`.
- Produces:
```cpp
class Sidebar : public QWidget {
    Q_OBJECT
public:
    struct NavItem { QString label; QString icon; int panel; };
    // workspaces in order, then the Settings item anchored at the bottom; historyPanel = the panel
    // under which the chat list shows. Ctrl+(panel+1) shortcuts are created for every item.
    Sidebar(const QList<NavItem> &workspaces, const NavItem &settings, int historyPanel, QWidget *parent = nullptr);
    void setCurrentPanel(int panel);      // checks that button, shows/hides the chat list
    void refreshIcons();                  // nav icons + the list's icons, for the current theme
    ChatHistoryList *history() const;
    StudioCanvas *brandSeal() const;      // objectName "brandSeal" is not required; property brandMark=true
signals:
    void panelRequested(int panel);
};
```
  Nav buttons keep objectName `navBtn`, property `panel`, `navicon`, checkable, tooltip `"<label> · Ctrl+<panel+1>"`. The sidebar widget keeps objectName `sidebar`, fixed width 176. Layout as today (margins 14,20,14,16; spacing 6; the brand seal 76px with the wordmark and "LOCAL STUDIO" tag below; "WORKSPACES" tag; nav column spacing 8; chat list under the history panel's button with left margin 10; stretch; Settings last).

- [x] **Step 1: Write the failing checks** (`#include "widgets/sidebar.h"`; build one `Sidebar` with the six items exactly as `MainWindow` uses: Library/box/0, Training/graduation-cap/1, Knowledge/database/2, Adapters/layers/3, Playground/message-circle/4, Settings/settings/5, historyPanel 4)
  - "sidebar: six nav buttons in order with today's tooltips": labels via `button->property("navicon")` count 6; tooltips `"Library · Ctrl+1"` … `"Settings · Ctrl+6"`; all checkable; sidebar `width()==176` after `show()`.
  - "sidebar: clicking a button asks for its panel": spy on `panelRequested`, click Training → `{1}`.
  - "sidebar: Ctrl+1..6 ask for their panel": for each `QShortcut` found by `findChildren<QShortcut *>()` with key `Ctrl+N`, `emit activated()` → `panelRequested(N-1)`; all six exist.
  - "sidebar: setCurrentPanel checks one button and shows the list only on Playground": for panel 4 only Playground is checked and `history()->isVisibleTo(&sidebar)`; for panel 1 only Training checked and the list hidden.
  - "sidebar: icons follow the theme": for `dark` in {true,false}: `Theme::setDark(dark); sidebar.refreshIcons();` the checked button's icon image differs from an unchecked button's icon image.
  - "sidebar: fits 176x680 with a populated list": `history()->setChats(7 summaries, "")`, `setCurrentPanel(4)`, `resize(176,680)`, pump; every `navBtn` and the list lie inside `rect()` and none overlap (no `geometry().intersects` between neighbours), Settings button bottom ≤ 680.
  - "sidebar: brand seal is the animated brand mark": `brandSeal()->property("brandMark").toBool()`.
- [x] **Step 2: Run to verify failure** — `cmake --build build -j 2>&1 | tail -5` — Expected: `sidebar.h` not found.
- [x] **Step 3: Implement `Sidebar`** by moving the code now in `MainWindow::setupUi()` (brand block, `makeNavButton`, nav list with the history slot, Settings) and `refreshNavIcons()` into `app/src/widgets/sidebar.cpp`. Shortcuts are created as children of the `Sidebar` (WindowShortcut context) and `click()` their button. `setCurrentPanel` is the body of today's check-loop in `MainWindow::switchPanel` plus `m_history->setVisible(panel == historyPanel)` plus `refreshIcons()`.
- [x] **Step 4: Switch `MainWindow` over.** Create `m_sidebar = new Sidebar(...)` with the same six items using the existing `Panel` enum; `connect(m_sidebar, &Sidebar::panelRequested, this, &MainWindow::switchPanel)`; `m_history = m_sidebar->history()`; the dark-mode toggle slot and `switchPanel` call `m_sidebar->refreshIcons()` / `setCurrentPanel(index)`; `applyAppearance` uses `m_sidebar->brandSeal()`; the end of `setupUi()` calls `m_sidebar->setCurrentPanel(m_stack->currentIndex())`. Delete `m_sideButtons`, `refreshNavIcons()` (header too) and the leftover `connection`-to-nav code that moved. The `connection` frame, `m_statusPill`, `m_connectionDot`, `mode` stay in `MainWindow` (top bar).
- [x] **Step 5: Run to verify pass** — `cmake --build build -j && ./build/magnite-app --ui-check 2>&1 | grep "sidebar:"` — Expected: all `ok:`; whole-run failures equal the baseline; `./build/magnite-app --theme-check 2>&1 | tail -1` shows 0 failures; `./build/magnite-app --selftest` prints `[selftest] PASS (0 failures)`.
- [x] **Step 6: Eyeball** `MAGNITE_SCREENSHOT_COMPACT=1 ./build/magnite-app --screenshot "$SCRATCH/t2"` and compare `panel-4.png` with the pre-change look (brand block, nav, list). Note: Library is now highlighted at start-up if nothing was before; that is accepted.
- [x] **Step 7: Record progress.**

---

### Task 3: Collapsed presentation, toggle button, Ctrl+B, remembering

**Files:**
- Modify: `app/src/widgets/sidebar.{h,cpp}`, `app/resources/theme/console.qss`, `app/src/testing/uicheck.cpp`

**Interfaces:**
- Consumes: `SidebarState` (Task 1), `Sidebar` (Task 2).
- Produces (added to `Sidebar`):
```cpp
    bool isCollapsed() const;
    void setCollapsed(bool collapsed, bool animate = false);  // applies; does NOT save
    void toggle();                                            // flips, saves via SidebarState::save(), emits collapsedChanged
signals:
    void collapsedChanged(bool collapsed);
```
  The constructor starts from `SidebarState::load()` and applies it immediately (no animation, correct width from the first paint). The toggle is a `QPushButton` objectName `sidebarToggle`, 24–28px, icon `chevron-left` when expanded and `chevron-right` when collapsed (tinted `textSecondary`, refreshed in `refreshIcons()`), tooltip from `SidebarState::toggleTooltip()`, accessible name "Collapse sidebar"/"Expand sidebar". Placement: expanded, top-right beside the 76px seal in the same row (the expanded height budget must not grow); collapsed, centred under the small seal (28–32px, same `StudioCanvas`, resized). A `QShortcut(Qt::CTRL|Qt::Key_B)` with `Qt::WindowShortcut` context calls `toggle()`.
  Collapsed presentation: the sidebar gets property `collapsed="true"` (re-polish), fixed width 52, layout margins about 8; nav buttons text cleared and `accessibleName` set to the label (label kept in a property `label`), fixed about 36 wide, `Qt::AlignHCenter`; the wordmark, both `studioTag` labels and the chat list hidden. QSS `QWidget#sidebar[collapsed="true"] QPushButton#navBtn` removes text padding, centres the icon; the checked state shows an accent bar on the left edge plus the `@surfaceFill` highlight (for example `border-left: 3px solid @accentPrimary` kept only if it fits, else an inset bar), hover as today.

- [x] **Step 1: Write the failing checks** (`Sidebar` as in Task 2, animation not involved; remove the setting key at the end)
  - "sidebar: collapsing makes a 52px icon strip": `setCollapsed(true)` → `width()==52`, `minimumWidth()==maximumWidth()==52`, all six nav buttons' `text().isEmpty()`, `accessibleName()` equals the label ("Library", … "Settings"), tooltips unchanged ("Library · Ctrl+1"), every `studioTag` label and the wordmark `!isVisibleTo(&sidebar)`, `brandSeal()` side ≤ 32.
  - "sidebar: the chat list stays hidden while collapsed": collapsed, `setCurrentPanel(4)` → `!history()->isVisibleTo(&sidebar)`; expanding shows it again; expanding restores the labels as text and `width()==176`.
  - "sidebar: the strip fits at 52x680 in both themes": for dark in {true,false}: `resize(52,680)`, every nav button and the toggle and seal inside `rect()`, no overlaps, Settings bottom ≤ 680.
  - "sidebar: the open workspace keeps the orange marker when collapsed, in both themes": `grab()` the sidebar image, scan the checked button's leftmost 5 pixel columns for `QColor(Theme::accentPrimary())`; present in both themes; absent for an unchecked button.
  - "sidebar: switching theme while collapsed re-tints the icons": toggle `Theme::setDark` then `refreshIcons()`; checked button's icon image changes and still differs from an unchecked button's.
  - "sidebar: the toggle button": tooltip/accessible names per state; `click()` flips `isCollapsed()`, emits `collapsedChanged` once, and `QSettings().value(SidebarState::kSettingsKey).toBool()` equals the new state; chevron icon image differs between states.
  - "sidebar: Ctrl+B toggles": `emit activated()` on the Ctrl+B shortcut flips the state; `shortcut->context()==Qt::WindowShortcut`; and a `ShortcutOverride` key event (Ctrl+B) sent to a `QPlainTextEdit` stays *ignored* ("Ctrl+B is not eaten by the chat box").
  - "sidebar: a saved collapsed state shows at once": `QSettings().setValue(key,true)`, construct a new `Sidebar`, `show()`; before any `pump`, `width()==52` and nav texts empty. Garbage value → expanded 176.
- [x] **Step 2: Run to verify failure** — build; Expected: compile error (`setCollapsed`, `toggle` missing).
- [x] **Step 3: Implement** the interface and presentation above in `sidebar.cpp`/`console.qss`. Use `SidebarState` for every decision (widths, `showLabels`, `showHistory`, tooltip). `setCollapsed` re-polishes the sidebar and every nav button (`unpolish/polish`) and calls `refreshIcons()`.
- [x] **Step 4: Run to verify pass** — `cmake --build build -j && ./build/magnite-app --ui-check 2>&1 | grep -E "sidebar"` all `ok:`; whole-run failures at baseline; `--theme-check` 0 failures.
- [x] **Step 5: Record progress.**

---

### Task 4: + New chat button, busy state, focus, slide animation

**Files:**
- Modify: `app/src/widgets/sidebar.{h,cpp}`, `app/resources/theme/console.qss`, `app/src/testing/uicheck.cpp`

**Interfaces:**
- Consumes: `Sidebar` (Tasks 2–3), `ChatHistoryList::setBusy(bool)`.
- Produces (added to `Sidebar`):
```cpp
    void setBusy(bool busy);                 // forwards to history()->setBusy(busy) AND enables/disables the + button
    void setAnimationsEnabled(bool enabled); // default true; MainWindow turns it off in verification runs
signals:
    void newChatRequested();                 // emitted by the + button
```
  The + button: `QPushButton` objectName `sidebarNewChat`, round about 26px, outlined (`@surfaceBorder`) on `@surfaceFill` with the `plus` icon tinted `@accentLabel` (`@textMuted` when disabled), tooltip and accessible name "New chat", directly under the Playground (history panel) button, visible only when `SidebarState::showNewChatButton(currentPanel == historyPanel)`. `toggle()` animates the fixed width from the current to the target over about 180 ms (`QVariantAnimation` driving `setFixedWidth`) unless animations are disabled or `appearance/studio3ReducedMotion` is true; on collapse the labels are hidden at the start of the slide, on expand they appear at the end. A new toggle during a slide stops the old one and starts from the current width. Collapsing moves keyboard focus off the chat list to the Playground button if (and only if) the focus was inside the list.

- [x] **Step 1: Write the failing checks**
  - "sidebar: + shows only when collapsed and Playground is open": all four combinations of (collapsed, panel 4 vs panel 1) via `isVisibleTo(&sidebar)`; geometry: when shown, its top is below the Playground button's bottom and above the Settings button's top, and it is horizontally centred within 2px.
  - "sidebar: + asks for a new chat": spy on `newChatRequested`, `click()` → one emission; tooltip and accessible name "New chat".
  - "sidebar: + and the list's New chat row always agree": `setBusy(true)` → `!sidebarNewChat->isEnabled()` and the `historyNew` button inside `history()` disabled; `setBusy(false)` → both enabled. Plus a streaming case reusing the feature-3 fixture pattern (`fixture.holdChat`, a `ChatPanel` with a `ChatStore`, `connect(&chat,&ChatPanel::busyChanged,&sidebar,&Sidebar::setBusy)`): both disabled while the reply streams and enabled after Stop.
  - "sidebar: collapsing moves focus off the hidden chat list": with the list's inner `QListWidget` focused (`setFocus()` on a shown sidebar), collapse → `sidebar.focusWidget()` is the Playground button; with focus on the Library button before collapsing it stays on Library.
  - "sidebar: the slide ends exactly at 52 and 176": `setAnimationsEnabled(true)`, reduced motion off; `toggle()`; right after the call width is not yet 52; `waitUntil(width()==52, 1000)`, widths seen while sliding stay within 52..176; expand likewise ends at 176 with texts back.
  - "sidebar: reduced motion snaps": `QSettings().setValue("appearance/studio3ReducedMotion", true)`, `toggle()` → `width()==52` immediately; remove the key afterwards.
  - "sidebar: two quick toggles end in the last requested state": `toggle(); toggle();` mid-slide, wait → `isCollapsed()==false`, `width()==176`, saved setting false; then `toggle(); toggle(); toggle();` → collapsed, 52.
- [x] **Step 2: Run to verify failure** — build; Expected: compile errors.
- [x] **Step 3: Implement** per the interface. `setBusy` is the single place that touches both buttons. Refresh the + icon in `refreshIcons()` and on enabled changes.
- [x] **Step 4: Run to verify pass** — `cmake --build build -j && ./build/magnite-app --ui-check 2>&1 | grep -E "sidebar"` all `ok:`; failures at baseline.
- [x] **Step 5: Record progress.**

---

### Task 5: Wire it into `MainWindow`

**Files:**
- Modify: `app/src/mainwindow.{h,cpp}` (`setupUi`, `captureScreenshots`, `runSelfTest`)

**Interfaces:**
- Consumes: `Sidebar` API from Tasks 2–4.
- Produces: `MainWindow::m_sidebar` (`Sidebar *`) used by the selftest and screenshots.

- [x] **Step 1: Write the failing selftest checks** in the base part of `MainWindow::runSelfTest` (near the "navigated through all panels" checks, using `checkC`):
  - "sidebar: toggle button collapses to 52px and restores 176px": click `sidebarToggle` found under `m_sidebar`, `m_sidebar->width()==52`, click again `==176`.
  - "sidebar: + under Playground starts a new chat": `switchPanel(PanelChat)`, collapse, the + is visible, `click()`; `m_chatPanel->currentChatId().isEmpty()` and no crash; expand again. `--selftest` uses the real `QSettings`, so read `SidebarState::load()` at the start of these checks and write it back with `save()` at the end, leaving the user's stored choice as it was.
  - "sidebar: + follows the chat's busy state": `emit m_chatPanel->busyChanged(true)` disables the + and the list's New chat row; `busyChanged(false)` re-enables both.
- [x] **Step 2: Run to verify failure** — `cmake --build build -j && ./build/magnite-app --selftest 2>&1 | grep -E "sidebar|PASS|FAIL"` — Expected: the three new lines fail.
- [x] **Step 3: Wire:**
  - `connect(m_sidebar, &Sidebar::newChatRequested, m_history, &ChatHistoryList::newChatRequested);` so the existing `ChatHistoryController` handles it.
  - `connect(m_chatPanel, &ChatPanel::busyChanged, m_sidebar, &Sidebar::setBusy);` and `m_sidebar->setBusy(m_chatPanel->isBusy());` after the controller is created.
  - `m_sidebar->setAnimationsEnabled(!verificationRun());`
  - Screenshots: in `captureScreenshots`, if env `MAGNITE_SCREENSHOT_SIDEBAR=collapsed` then `m_sidebar->setCollapsed(true)` (this does not save to settings).
- [x] **Step 4: Run to verify pass** — `./build/magnite-app --selftest 2>&1 | tail -3` — Expected: `[selftest] PASS (0 failures)`; `./build/magnite-app --ui-check` at baseline.
- [x] **Step 5: Record progress.**

---

### Task 6: Look, full suites, docs, review, commit

**Files:**
- Modify: `docs/development/architecture.md` (new short "Collapsible sidebar" section after "Chat history": `SidebarState` / `Sidebar`, the setting key, 176/52, Ctrl+B, + button and busy state, animation rules, `MAGNITE_SCREENSHOT_SIDEBAR`), this plan (ticks)

- [x] **Step 1: Make a populated chat folder.** In the scratchpad, write a throwaway script that creates `$SCRATCH/home/chats/<uuid>.json` for 6 chats: `{"version":1,"id","title","customTitle":true,"created","updated","tree":{"nodes":[{"id":1,"parent":0,"role":"user","content":"hi","children":[2],"activeChild":2,"notes":[],"compaction":{"summary":"","covered":0}},{"id":2,"parent":1,"role":"assistant","content":"hello","children":[],"activeChild":0,"notes":[],"compaction":{"summary":"","covered":0}}],"rootChildren":[1],"rootActive":1}}` with timestamps in the format `ChatStore` writes (copy `stamp()`'s format). If the list shows no chats, fix the fixture, not the app.
- [x] **Step 2: Screenshots** for light and dark (toggle with the stored `appearance/studio3Dark`, or run once per theme), expanded and collapsed, at 1024x680:
  `MAGNITE_HOME=$SCRATCH/home MAGNITE_SCREENSHOT_COMPACT=1 [MAGNITE_SCREENSHOT_SIDEBAR=collapsed] ./build/magnite-app --screenshot $SCRATCH/shots-<state>-<theme>`. Look at `panel-4.png` (Playground: list or +) and one other panel for each: nothing clipped, marker visible, icons crisp, strip background/border match, no leftover text. Fix what looks wrong (return to the relevant task's tests first). Send the user the four Playground images.
- [x] **Step 3: Full suites.** `cmake --build build -j`; `./build/magnite-app --ui-check` (run twice if the first shows extra timing failures; expect only the known keyboard-selection failures), `./build/magnite-app --theme-check` (0 failures), `./build/magnite-app --selftest` (PASS), `PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests` (228 OK). Report the counts honestly.
- [x] **Step 4: Docs** note in `architecture.md`.
- [x] **Step 5: One fresh reviewer** (Agent, `superpowers:requesting-code-review` style) reads the whole change (`git diff` plus the new files, the spec and this plan) and reports; fix real findings with a test first, re-run the suites, say what was skipped and why.
- [x] **Step 6: Commit locally** on `feature`: `git add app docs/superpowers/specs/2026-11-07-collapsible-sidebar-design.md docs/superpowers/plans/2026-11-07-collapsible-sidebar.md docs/development/architecture.md` then `git commit -m "feat(ui): feature 4/5 - collapsible sidebar"` with no trailers. Do not push. Update the memory note `feature-roadmap-2026-11` (feature 4 done, hash) and tell the user.
- [x] **Step 7: Record progress.**
