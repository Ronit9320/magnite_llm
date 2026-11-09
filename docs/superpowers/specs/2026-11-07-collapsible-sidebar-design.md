# Collapsible sidebar (feature 4/5)

## Why

The left menu is a fixed 176px column that is always on screen. People want more room for the chat (or any other workspace) and want the menu out of the way when they are not navigating, the way the sidebar on Claude's website folds into a slim strip and opens again with one click.

## What the user sees

- **A collapse button** at the top of the sidebar, next to the logo/name. Clicking it folds the sidebar into a slim strip; clicking the same button (now showing the opposite arrow) opens it again.
- **Expanded** looks as it does today: logo, "MAGNITE" name, "LOCAL STUDIO" tag, "WORKSPACES" label, the five workspace buttons (Library, Training, Knowledge, Adapters, Playground), the saved-chats list under Playground (only while Playground is open), and Settings. (The connection indicator and the light/dark toggle live in the window's top bar, not the sidebar, and stay there untouched in both states.)
- **Collapsed** is a strip about 52px wide (my starting guess, see "Strip width" below) with:
  - a small logo mark with the toggle button under it,
  - the six icons (five workspaces + Settings), icon only, no text; the open workspace keeps its orange marker and highlighted background,
  - while Playground is the open workspace, a small round **+ (New chat)** directly under the Playground icon, with the tooltip "New chat". It starts a new chat exactly like the list's "New chat" row,
  - nothing else: the strip has no connection dot (the top bar already shows the connection, always).
- **Hidden while collapsed:** the saved-chats list, the "WORKSPACES" label, "LOCAL STUDIO" and the wordmark. The list stays hidden even when Playground is opened or re-opened.
- **The active marker may change shape in the strip** (for example an accent bar on the left edge of the highlighted square instead of today's 3px left border), because the narrow width cannot hold today's padding. It must still clearly be the orange accent.
- **Hovering an icon** in the strip shows the same tooltip as now ("Library · Ctrl+1"). Nothing about the tooltips changes in either state.
- **Keyboard:** Ctrl+1 to Ctrl+6 work in both states, exactly as now. **Ctrl+B** toggles the sidebar (my call; no other binding in the app uses it). The toggle button has a tooltip naming the action and the shortcut ("Collapse sidebar · Ctrl+B" / "Expand sidebar · Ctrl+B") and an accessible name.
- **It remembers.** The state is stored in `QSettings` (`appearance/sidebarCollapsed`, my call on the key name) and restored on the next launch with no visible animation at start-up.
- **Motion.** Changing state slides the width over a short time (about 180ms, my call). With "reduced motion" on in Settings (`appearance/studio3ReducedMotion`), and in `--ui-check`/`--selftest`/`--screenshot` runs, it snaps instantly.
- Works in light and dark themes; the strip uses the same sidebar background and border tokens as the wide sidebar.

## Goals

- The two states look intentional and finished in both themes at the minimum window size (the window's minimum height is 680).
- Nothing the sidebar offers becomes unreachable: every workspace, Settings, and "New chat" can be used while collapsed.
- The collapsed/expanded choice survives a restart.
- No change to what the panels show; they simply get the extra width when the sidebar is collapsed.

## Non-goals

- Resizing the sidebar by dragging its edge, or any width other than the two fixed ones.
- Opening the sidebar temporarily on hover, or a hidden-completely mode.
- Showing saved chats while collapsed (they return when expanded); no pop-up chat list from the strip.
- Auto-collapsing at narrow window widths.
- Changes to the saved-chats list itself, its rows, the greeting, or any backend route.

## Anti-goals (a failure even if it technically works)

- Keyboard focus left on a widget that has just been hidden (for example the chat list) so that the next Tab goes nowhere sensible. When collapsing, focus must move to a visible control.
- The + in the strip being usable at a moment when the list's "New chat" row would be disabled (a reply streaming, a send being prepared, a model loading, a message editor open). The two must always agree.
- Icons or the active marker looking wrong after a theme switch while collapsed (stale tint, missing marker).
- The strip layout breaking when the window is at its minimum height (six icons, the +, the logo and the toggle must fit without clipping).
- A start-up flash of the wrong state (expanded for a moment, then snapping to collapsed).
- Existing tests, tooltips, accessible names and shortcuts regressing.

## How it works

- **Same widgets, two presentations (my call).** The existing nav buttons are kept and switch between "icon + text" and "icon only" rather than building a second set of buttons, so shortcuts, checked state, tooltips and theme icon-tinting (`refreshNavIcons`) continue to work unchanged. In the collapsed state the buttons' text is hidden (the label text is kept in the button, so the tooltip and accessible name still use it), and the QSS gets a collapsed variation keyed on a property on the sidebar (for example `collapsed="true"`); the colours come only from existing tokens, or new tokens added to `theme.h` and checked in `Theme::selfCheck()`.
- **The sidebar becomes its own widget (my call, needed for testing).** `--ui-check` cannot construct a `MainWindow`, and today the sidebar is built inline in `MainWindow::setupUi()`. The brand block, nav buttons, history slot, + button, toggle, Settings button and nav-icon tinting move into a new `Sidebar` widget (own `.cpp/.h`, in `MAGNITE_SOURCES`) that `MainWindow` creates. The `Sidebar` creates the `ChatHistoryList` under Playground as today, and `MainWindow` gets it from the sidebar (an accessor) to give to the `ChatHistoryController`. The sidebar tells the window which workspace was requested (signal) and is told which one is open (slot). The existing behaviour (checked state, tooltips, Ctrl+1–6 shortcuts, icon tints, history shown only on Playground) moves with it unchanged. The UI check can then build a real `Sidebar` without a backend.
- **A small testable model.** The decisions live in a plain class with no widgets, for example `SidebarState`: whether it is collapsed, toggle/set, the target width for each state, what is visible for a given open workspace, reading and writing the stored setting, the toggle's tooltip text. `Sidebar` applies what it says.
- **The + button** belongs to the `Sidebar`, sits under the Playground button, is visible only when collapsed and Playground is the open workspace, and requests a new chat the same way the list's "New chat" row does. Its enabled state follows the same `ChatPanel::busyChanged` signal that already drives `ChatHistoryList::setBusy` (my call: `MainWindow` connects it to the sidebar as well, and starts it enabled like the list), so the two cannot disagree. `ChatHistoryList` itself does not change.
- **Ctrl+B** is a window-wide shortcut that works even while the chat box has focus (the chat box is plain text, there is no bold).
- **Focus.** If the focused widget is hidden by collapsing (the chat list), focus moves to the Playground button (or the toggle button); on expanding, nothing steals focus from where the user was.
- **The width change** animates the sidebar's fixed width (the current code sets a fixed width of 176; the builder chooses how to animate that cleanly) and settles with the correct final widths: 176 expanded, the strip width collapsed. During the slide, text must not wrap or jump awkwardly (the builder may hide the labels at the start of collapsing and show them at the end of expanding).
- **Brand block.** The 76px logo seal and wordmark become a small mark (about 28–32px, the builder's call on reusing the existing logo canvas or the brand SVG) in the strip. The seal's motion setting still applies.
- **Strip width.** 52px is my starting guess from the mockup the user chose; the builder may adjust a few pixels so the 18px icons, the active marker and the focus ring fit properly.

## Constraints

- C++20/Qt6; no hard-coded colours (tokens in `app/src/theme.h`, `Theme::selfCheck()` extended for any new ones); reuse existing Lucide-style icons where possible (`chevron-left.svg` and `chevron-right.svg` exist for the toggle; `plus.svg` exists for New chat); new `.cpp` files go in `MAGNITE_SOURCES` in `app/CMakeLists.txt`.
- No backend change; the backend suite stays at 228 tests. No `DATA_SCHEMA` change (a UI preference in `QSettings` is not a persistent data format).
- The minimum window size (height 680) is unchanged. The expanded sidebar's content, including the chat list shrinking to two rows in a short window, behaves as it does today.

## Left to the builder

- Exact strip width, icon sizes, the + button's look (it should echo the mockup: a small round outlined button with the accent-coloured plus), hover/focus styling, animation implementation, the name and shape of the model class, and where the toggle sits in each state (top-right of the brand block expanded, under the small mark when collapsed).
- How the stored state is read at start-up so there is no flash.

## Verification

- Failing tests first, then implementation. `--ui-check` additions: the model class (toggle, persistence of the setting through `QSettings`, widths, what is visible in each state for each open workspace, tooltip text); the strip pieces in both themes (sizes, icons tinted, active marker, tooltips and accessible names present, text hidden but label kept); the + button's enabled state following the busy signal (and the chat list staying hidden while collapsed); Ctrl+B and Ctrl+1–6 handlers where they can be constructed; theme tokens in `Theme::selfCheck()`.
- A visual check of the real window: `MAGNITE_SCREENSHOT_COMPACT=1 ./build/magnite-app --screenshot <dir>` in expanded and collapsed states, light and dark, with a populated chat folder (set `MAGNITE_HOME`), including the minimum height.
- Full suites: `--ui-check` back to its known baseline (about 586 plus the new checks, only the known keyboard-selection failures), `--theme-check` 0 failures, backend 228 OK.
- One fresh reviewer looks at the whole change at the end.
