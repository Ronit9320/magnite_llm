# Editing Sent Prompts, Retry and Version Switching Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the user edit any message they sent and re-run the chat from there, retry the latest reply, and flip between the versions of an edited message or retried reply with a `‹ n / total ›` switcher.

**Architecture:** Client-only. The flat `m_history` + `m_compaction` in `ChatPanel` become a small pure-logic `ChatTree` (one node per message, siblings = versions, one active path; compaction snapshots and transcript notes live on nodes). The transcript becomes a drawing of the active path made of `MessageRow` widgets (bubble + hover action + version switcher + inline editor). Edit and retry reuse the existing send flow (measure → maybe compact → stream) with the conversation temporarily ending at the edit point.

**Tech Stack:** C++20 / Qt6 Widgets, QSettings, loopback fixture server and `check(...)` harness in `app/src/testing/uicheck.cpp` (`./build/magnite-app --ui-check`).

**Spec:** `docs/superpowers/specs/2026-11-06-edit-prompts-design.md`

## Global Constraints

- No hex colours in widget code or QSS (AGENTS.md). Existing tokens (`@surfaceFillHover`, `@surfaceBorder`, `@textPrimary`, `@textSecondary`, `@insetBackground`, `@accentPrimary`) are enough; if a new colour is needed, add a token in `app/src/theme.h` and extend `Theme::selfCheck()`.
- New C++ files go in `MAGNITE_SOURCES` in `app/CMakeLists.txt`; new icons go in `app/resources.qrc` and in the `kIcons` list in `Theme::selfCheck()` (`app/src/theme.cpp`). Icons are Lucide-style SVG with `stroke="currentColor"`, tinted with `Theme::coloredIcon()`.
- Switcher text is exactly `n / total` (e.g. `2 / 2`) flanked by `‹` `›` buttons; shown only when a message has more than one version; always visible then. Pencil (user rows) and retry (latest assistant row only) appear on hover.
- Edit editor: `Esc` cancels, `Cmd/Ctrl+Enter` saves, buttons "Cancel" and "Save & send". Empty (after trim) or unchanged text = Cancel. Accessible names: "Edit message", "Retry reply", "Previous version", "Next version".
- Pencils, retry and arrows are disabled while a reply is streaming, while a send is being prepared (`m_preparing`), and while a model is loading.
- Nothing visible or stored changes until a send actually begins (`startSend`). Cancelled compaction → editor reopens with the text.
- Bubbles removed from the layout must use `setParent(nullptr)` + `deleteLater()` (as `onClearChat` does) so `findChildren<ChatBubble *>()` counts stay exact.
- Backend is untouched. Backend suite must stay at 228 tests OK: `PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests`.
- All changes stay local and **uncommitted** (standing user instruction) — every task's final step is "Record progress": tick that task's checkboxes in this file.
- Baseline: `./build/magnite-app --ui-check` has exactly 4 pre-existing failures ("training/settings dropdown supports keyboard selection"). Record the exact output tail in Task 1 Step 1; after every task the failure list must be identical (plus nothing new). Build: `cmake --build build -j`.

## Review Focus

1. Edit or retry while the backend is not ready — status error shown, the editor stays open with the typed text, tree unchanged; with the backend ready but no model loaded — behaves like the composer today (send starts, error shown, a failed retry keeps the old reply, a failed edit leaves a reply-less new version beside the original). (Tasks 5, 7)
2. Retry that returns no text (error or empty), and Stop pressed before any text — previous reply stays, switcher unchanged; Stop after partial text keeps the partial as a new version. (Task 7)
3. Clear chat while an editor is open, while compacting an edit, or while a retry streams — no crash, no stale rows, composer usable. (Tasks 5, 7)
4. Editing the first message (parent is the root) and a message inside an older version (branches again); whitespace-only, unchanged, multi-line and non-ASCII edits. (Tasks 1, 5)
5. The draft typed in the main composer survives edit, retry and switching untouched. (Tasks 5, 6, 7)

---

## File Structure

| File | Responsibility |
|---|---|
| `app/src/panels/chattree.{h,cpp}` (new) | `ChatTree`: pure message tree, active path, versions, notes, compaction snapshots. No widgets. |
| `app/src/widgets/versionswitcher.{h,cpp}` (new) | `VersionSwitcher`: `‹ n / total ›` widget. |
| `app/src/panels/messagerow.{h,cpp}` (new) | `MessageRow`: one bubble + hover action + switcher + inline editor. |
| `app/src/panels/chatpanel.{h,cpp}` (modify) | Owns the tree; renders the path; unified send flow for new/edit/retry; switching. |
| `app/src/panels/chatbubble.{h,cpp}` (unchanged unless needed) | Bubble stays as is; rows wrap it. |
| `app/resources/icons/{pencil,chevron-left,chevron-right}.svg`, `app/resources.qrc`, `app/src/theme.cpp` (modify) | Icons + self-check list. |
| `app/resources/theme/console.qss` (modify) | Row, action button, switcher and editor styles, tokens only. |
| `app/src/testing/uicheck.cpp` (modify) | Tree-logic checks, fixture knobs, panel checks. |
| `app/CMakeLists.txt` (modify) | Add new sources. |
| `docs/development/architecture.md` (modify) | Short note on the tree + row rendering. |

---

### Task 1: `ChatTree` pure logic

**Files:**
- Create: `app/src/panels/chattree.h`, `app/src/panels/chattree.cpp`
- Modify: `app/CMakeLists.txt` (add both to `MAGNITE_SOURCES`), `app/src/testing/uicheck.cpp` (checks next to the existing `ContextMeterLogic` checks)

**Interfaces:**
- Produces (all later tasks rely on these exact names):
```cpp
class ChatTree {
public:
    using NodeId = int;                         // 0 = virtual root; real ids start at 1 and are never reused until clear()
    static constexpr NodeId kRoot = 0;
    struct Compaction { QString summary; int covered = 0; };   // covered = leading path messages replaced by summary

    NodeId addChild(NodeId parent, const QString &role, const QString &content); // newest version; becomes parent's active child
    NodeId parentOf(NodeId id) const;
    QString role(NodeId id) const;
    QString content(NodeId id) const;
    QList<NodeId> activePath() const;           // root excluded, oldest first; follows each node's remembered active child
    int pathIndexOf(NodeId id) const;           // -1 when not on the active path
    NodeId lastOnPath() const;                  // kRoot when empty
    QList<NodeId> versionsOf(NodeId id) const;  // siblings including id, creation order
    int versionIndex(NodeId id) const;          // 0-based within versionsOf
    int versionCount(NodeId id) const;
    void activate(NodeId id);                   // make id its parent's active child (its own remembered continuation returns with it)
    QList<QJsonObject> messages(int pathCount = -1) const;   // {role,content} of the first pathCount path nodes; -1 = all
    void addNote(NodeId id, const QString &text);
    QStringList notes(NodeId id) const;
    void setCompaction(NodeId id, const Compaction &c);
    Compaction compaction(int pathCount = -1) const;   // snapshot on the LAST of the first pathCount path nodes that carries one; covered==0 when none
    bool isEmpty() const;
    void clear();
};
```

- [ ] **Step 1: Record the baseline.** Run `cmake --build build -j && ./build/magnite-app --ui-check 2>&1 | tail -15` and `PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests 2>&1 | tail -3`. Paste the failure lines and test counts under this step as the baseline. Expected: the 4 known failures; 228 OK.
- [ ] **Step 2: Write failing checks** (named exactly): "chat tree: an empty tree has an empty path and no messages"; "chat tree: messages added in turn form the path in order"; "chat tree: a sibling becomes the newest, active version and counts n of n"; "chat tree: activating an older version restores its whole continuation"; "chat tree: editing the first message branches from the root"; "chat tree: an edit inside an older version branches again"; "chat tree: messages(pathCount) returns only the leading messages"; "chat tree: compaction is the last snapshot on the path"; "chat tree: a snapshot on a node that left the path no longer applies, and returns when it is switched back"; "chat tree: compaction(pathCount) ignores nodes at or after pathCount"; "chat tree: notes stay with their node"; "chat tree: clear resets everything". Each builds a small `ChatTree` and asserts ids/roles/contents/indices (e.g. after two edits of message 1, `versionCount==3` and `versionIndex==2`).
- [ ] **Step 3: Run it to verify it fails** (build error: `chattree.h` not found). Run: `cmake --build build -j 2>&1 | tail -5`.
- [ ] **Step 4: Implement `ChatTree`** in `chattree.cpp`. Approach: `QVector<Node>` indexed by `id-1` plus a root `children`/`activeChild`; node = `{parent, role, content, children, activeChild, notes, compaction}`; `addChild` appends to `children` and sets the parent's `activeChild`; `activate(id)` sets the parent's `activeChild` only (descendants keep their own remembered `activeChild`, which is what restores the continuation).
- [ ] **Step 5: Run to verify it passes.** Run: `cmake --build build -j && ./build/magnite-app --ui-check 2>&1 | grep -E "chat tree|failure"` → all 12 "chat tree" checks pass; total failures = baseline.
- [ ] **Step 6: Record progress.**

---

### Task 2: Move `ChatPanel` onto the tree (behaviour preserved, one intentional edge change)

**Files:**
- Modify: `app/src/panels/chatpanel.h`, `app/src/panels/chatpanel.cpp`

**Interfaces:**
- Consumes: `ChatTree` (Task 1).
- Produces (in `ChatPanel`, private):
```cpp
ChatTree m_tree;                                  // replaces QList<QJsonObject> m_history and Compaction m_compaction
struct PendingSend { enum Kind { NewMessage, Edit, Retry } kind = NewMessage;
                     ChatTree::NodeId anchor = ChatTree::kRoot;  // Edit: node being replaced; Retry: reply being replaced
                     QString text; };             // NewMessage/Edit: the user text; Retry: the user message's text
std::optional<ChatTree::Compaction> m_pendingCompaction;   // result of a compaction not yet committed
QStringList m_pendingNotes;                       // dividers to attach to the node created by the pending send
int prefixCount(const PendingSend &s) const;      // path messages that precede the new/replaced message: NewMessage = path size; Edit = pathIndexOf(anchor); Retry = pathIndexOf(anchor)  (reply excluded, its user message included)
QJsonArray effectiveMessages(int pathCount = -1) const;   // summary-plus-messages logic as today, over the first pathCount path messages (-1 = all)
```
`startSend(const QString &text)` becomes `startSend(const PendingSend &send)`; `measureThenSend` / `compactThenSend` take a `PendingSend` too. Add `#include <optional>` to the header.

- [ ] **Step 1: No new checks.** This is a pure refactor guarded by the existing feature-1 checks (message order, summary accumulation, divider text, clear chat forgets the summary, stop/restore).
- [ ] **Step 2: Refactor.** `onSend` builds `PendingSend{NewMessage, kRoot, text}`. `startSend` adds the user node under `m_tree.lastOnPath()`, then commits `m_pendingCompaction` (if set) with `m_tree.setCompaction(userNode, …)` and `m_pendingNotes` with `addNote` — but still draws bubbles and dividers exactly as today: the compaction response handler still calls `addContextDivider` immediately **and** also appends the same text to `m_pendingNotes` (recorded on the node, not yet used for drawing). That handler now computes `{summary or previous, base.covered + covered}` from `m_tree.compaction(prefixCount(send))` into `m_pendingCompaction` instead of mutating state; `effectiveMessages` and the `compactThenSend` slice read `m_tree.compaction(pathCount)` and `m_tree.messages(pathCount)`. Assistant text is added with `m_tree.addChild(userNode, "assistant", text)` **only when non-empty**, at the three places that today append to `m_history` (done event, `onDone` fallback, `onStop`). `onClearChat` calls `m_tree.clear()` and drops pending state. Note: this intentionally removes the old "empty assistant message after a `done` with no text" edge; call it out in the progress note.
- [ ] **Step 3: Build and run the full UI check.** Run: `cmake --build build -j && ./build/magnite-app --ui-check 2>&1 | tail -15`. Expected: failure list identical to the Task 1 baseline. If a feature-1 check fails, fix the refactor, not the check.
- [ ] **Step 4: Record progress** (note whether the empty-assistant edge changed any check).

---

### Task 3: `MessageRow` and rendering the transcript from the tree

**Files:**
- Create: `app/src/panels/messagerow.h`, `app/src/panels/messagerow.cpp`
- Modify: `app/CMakeLists.txt`, `app/src/panels/chatpanel.{h,cpp}`, `app/resources/theme/console.qss`

**Interfaces:**
- Consumes: `ChatTree`, `ChatBubble`.
- Produces:
```cpp
class MessageRow : public QWidget {            // object name "messageRow"
    Q_OBJECT
public:
    MessageRow(ChatBubble::Role role, ChatTree::NodeId node, const QString &text, QWidget *parent);
    ChatBubble *bubble() const;
    ChatTree::NodeId node() const;
    void setNode(ChatTree::NodeId node);       // the streaming row gets its node when the reply is committed
    // Task 4/5/6/7 add: setVersion, setActionsEnabled, setRetryAvailable, beginEdit, cancelEdit, signals.
};
void ChatPanel::renderTranscript(int pathCount = -1, const QStringList &trailingNotes = {});
// Clears everything except m_welcome, then for each of the first pathCount path nodes adds the node's notes
// (as "contextDivider" labels via addContextDivider) followed by a MessageRow (user: right-aligned, assistant: left-aligned).
// Then appends trailingNotes. Hides m_welcome when anything was drawn, calls ensureTrailingStretch(), and removes old
// widgets with setParent(nullptr) + deleteLater(). pathCount == -1 means "the whole path, or m_pathLimit when set" (Task 7).
// Does not add the streaming row.
```
`ChatPanel::addContextDivider` keeps its object name `contextDivider`.

- [ ] **Step 1: Write failing checks:** "messages render as rows, one per path message, bubble text matches" (send two turns on the fixture; `chat.findChildren<MessageRow *>().size()==4`, each `bubble()->text()` equals the message); "a row knows its node" (`row->node()!=0` and tree role matches `bubble()->role()`); "a divider is shown again when the transcript is redrawn" (compact once as in feature 1's flow, then call `renderTranscript()` through `QMetaObject::invokeMethod(&chat,"redrawForTest",Qt::DirectConnection)` and expect the same divider text; add the private slot `redrawForTest()` calling `renderTranscript()`). Run: expect build error / failures.
- [ ] **Step 2: Implement `MessageRow`** (a horizontal layout: stretch, bubble for user; bubble, stretch for assistant; QSS id `messageRow`, no visual change versus today) and `renderTranscript`. `startSend` now: add node, commit pending compaction/notes (as Task 2), then `renderTranscript()`, then add the streaming `MessageRow` for the assistant (no node yet), keeping `m_streamBubble = row->bubble()`. Dividers are no longer added immediately in the compaction handler: it pushes the divider text into `m_pendingNotes` (rendered by `renderTranscript` at `startSend`). On commit of the reply: `row->setNode(id)`.
- [ ] **Step 3: Run the full UI check.** Expected: new checks pass; failure list = baseline (the existing clear-chat checks cover the welcome panel).
- [ ] **Step 4: Record progress.**

---

### Task 4: Icons and `VersionSwitcher`

**Files:**
- Create: `app/resources/icons/pencil.svg`, `chevron-left.svg`, `chevron-right.svg`, `app/src/widgets/versionswitcher.{h,cpp}`
- Modify: `app/resources.qrc`, `app/src/theme.cpp` (`kIcons`), `app/CMakeLists.txt`, `app/resources/theme/console.qss`, `app/src/panels/messagerow.{h,cpp}`

**Interfaces:**
- Consumes: `MessageRow`.
- Produces:
```cpp
class VersionSwitcher : public QWidget {       // object name "versionSwitcher"
    Q_OBJECT
public:
    explicit VersionSwitcher(QWidget *parent = nullptr);
    void setVersion(int index, int total);     // index 0-based; text "index+1 / total"; hides itself when total <= 1; prev disabled at 0, next disabled at total-1
    void setEnabledForUse(bool enabled);       // false disables both arrows regardless of position
signals:
    void step(int delta);                      // -1 or +1
};
// buttons: "versionPrev" (accessible "Previous version"), "versionNext" ("Next version"), label "versionLabel"
// MessageRow gains: void setVersion(int index, int total); void setActionsEnabled(bool); signal versionStep(ChatTree::NodeId node, int delta);
// The switcher sits under the bubble: right-aligned for user rows, left-aligned for assistant rows.
```

- [ ] **Step 1: Write failing checks:** "icons: pencil, chevron-left, chevron-right are embedded" (add to `kIcons` in `Theme::selfCheck()` first and run `--theme-check`: expect 3 failures); "switcher: hidden for one version" (assert `isHidden()`, since the test widget has no shown parent); "switcher: shows '2 / 3' for index 1 of 3"; "switcher: previous disabled at the first, next at the last"; "switcher: step emits -1 and +1"; "switcher: disabled for use disables both arrows".
- [ ] **Step 2: Add the three Lucide SVGs** (stroke `currentColor`), register in `resources.qrc`, implement the widget (icon buttons via `Theme::coloredIcon`, QSS only with existing tokens), embed in `MessageRow`.
- [ ] **Step 3: Run `./build/magnite-app --theme-check` and `--ui-check`.** Expected: theme-check prints "0 failure(s)"; ui-check failures = baseline.
- [ ] **Step 4: Record progress.**

---

### Task 5: Edit a sent message

**Files:**
- Modify: `app/src/panels/messagerow.{h,cpp}`, `app/src/panels/chatpanel.{h,cpp}`, `app/resources/theme/console.qss`, `app/src/testing/uicheck.cpp`

**Interfaces:**
- Consumes: `PendingSend`, `renderTranscript`, `MessageRow::setActionsEnabled`.
- Produces:
```cpp
// MessageRow (user rows only)
void beginEdit(const QString &text);   // swaps the bubble for an inline editor (QPlainTextEdit "messageEditBox", buttons "editSave" = "Save & send", "editCancel" = "Cancel")
void cancelEdit();
bool isEditing() const;
// signals
void editSaved(ChatTree::NodeId node, const QString &text);
void editCancelled(ChatTree::NodeId node);
// pencil: QPushButton "editButton" (accessible "Edit message"), visible while the row is hovered (enter/leave events) and when the row's editor is not open.
// ChatPanel (private slots)
void onEditSaved(ChatTree::NodeId node, const QString &text);
void beginSend(const PendingSend &send);   // shared by onSend / onEditSaved / onRetry: backend-ready check, then measure→compact→startSend
```
Save semantics: trimmed text empty or equal to `m_tree.content(node)` → `cancelEdit()`. Otherwise `beginSend(PendingSend{Edit, node, text})`. On Save the editor closes and the original bubble returns until the send begins. `beginSend` decides whether to measure first with `prefixCount(send) - base.covered + (send.kind == Retry ? 0 : 1) > kKeepRecent` (the generalised form of today's `uncompacted + 1 > kKeepRecent`), and appends the user text to the messages it posts unless the kind is `Retry`. Opening an editor closes any other open one. `startSend` for `Edit`: `newNode = m_tree.addChild(m_tree.parentOf(anchor), "user", text)`, commit pending compaction/notes on `newNode`, `renderTranscript()`, streaming row. `prefixCount` for Edit = `pathIndexOf(anchor)` so usage/compact requests exclude the old continuation; the compaction slice posted = path messages from `base.covered` to that prefix plus the new text. If `cancelPreparing()` runs for an Edit, reopen that row's editor with the saved text (row looked up by node; skip silently if the row no longer exists). Backend not ready → status "Backend not connected." and the editor stays open.

- [ ] **Step 1: Write failing checks** (extend the feature-1 fixture block): "every message you sent has an edit pencil"; "hovering a row shows its pencil, leaving hides it" (send `QEnterEvent`/`QEvent::Leave`); "clicking the pencil opens the editor pre-filled and hides the bubble"; "opening a second editor closes the first"; "Esc cancels and changes nothing" (tree and request counts unchanged); "empty or unchanged text counts as cancel"; "Save & send on the user message of turn 2 in a 3-turn chat sends turn 1's two messages plus the edited text as the last user message" (assert the `fixture.chatRequests` body: roles and contents); "the screen shows the edited message and a fresh reply; the old continuation is gone from the screen"; "editing the first message branches from the root"; "the usage request for an edit excludes the old continuation" (`fixture.usageRequests`); "an edit at the compaction threshold compacts using only the messages before the edit point"; "Stop during that compaction reopens the editor with the typed text and leaves the conversation unchanged"; "with the backend not ready the editor stays open with its text"; "a multi-line, non-ASCII, padded edit round-trips with only outer whitespace trimmed"; "the draft in the main composer is untouched by editing"; "pencils are disabled while a reply is streaming" (`fixture.holdChat`); "clearing the chat while an editor is open leaves no rows"; "clearing the chat while an edit is being compacted cancels it and a late reply changes nothing" (`fixture.holdCompact` / `releaseHeldCompact`, as in feature 1). Add fixture knob `QString chatReply` (default the current fixed text) so tests can tell replies apart. Run: expect failures.
- [ ] **Step 2: Implement** per the Interfaces block. `Cmd/Ctrl+Enter` in the edit box saves (event filter in `MessageRow`); `Esc` cancels.
- [ ] **Step 3: Run the full UI check.** Expected: all new checks pass; failure list = baseline.
- [ ] **Step 4: Record progress.**

---

### Task 6: Switching versions

**Files:**
- Modify: `app/src/panels/chatpanel.{h,cpp}`, `app/src/panels/messagerow.{h,cpp}`, `app/src/testing/uicheck.cpp`

**Interfaces:**
- Consumes: `VersionSwitcher`, `ChatTree::activate`, `versionIndex/versionCount`.
- Produces: `void ChatPanel::onVersionStep(ChatTree::NodeId node, int delta)`; `void ChatPanel::refreshRows()` — for every `MessageRow`: `setVersion(index,count)` from the tree, `setActionsEnabled(!m_streaming && !m_preparing && !m_loading)`, and (Task 7) `setRetryAvailable`. Rows whose `node()` is 0 (the streaming row before its reply is committed) are skipped for versions. `updateActions()` calls `refreshRows()`.

`onVersionStep`: target = `versionsOf(node)[index+delta]`; ignore if out of range or busy; `m_tree.activate(target)`; `renderTranscript()` (keep the scroll position; do not force follow-output); `refreshRows()`; `refreshContextUsage()`.

- [ ] **Step 1: Write failing checks:** "an edited message shows 2 / 2 below its bubble"; "‹ switches back to the original message and its whole conversation, › returns to the edit and its continuation"; "after switching, the next send continues from the version on screen" (assert request body); "switching restores the compaction state of that version" (compact on the original, edit before the compaction, confirm the edited version sends no summary, switch back and confirm the summary returns); "dividers reappear after switching back"; "the usage request after a switch matches the screen"; "arrows are disabled while streaming and preparing"; "a version switch with several edits shows 3 / 3 and steps through all three"; "clear chat removes switchers"; "the draft in the main composer is untouched by switching".
- [ ] **Step 2: Implement** per the Interfaces block; connect `MessageRow::versionStep` in `renderTranscript` and the streaming row.
- [ ] **Step 3: Run the full UI check.** Expected: new checks pass; failure list = baseline.
- [ ] **Step 4: Record progress.**

---

### Task 7: Retry the latest reply

**Files:**
- Modify: `app/src/panels/messagerow.{h,cpp}`, `app/src/panels/chatpanel.{h,cpp}`, `app/src/testing/uicheck.cpp`

**Interfaces:**
- Consumes: `beginSend`, `PendingSend{Retry}`, `refreshRows`.
- Produces:
```cpp
// MessageRow (assistant rows)
void setRetryAvailable(bool available);    // "retryButton" (refresh-cw icon, accessible "Retry reply"), shown on hover only while available
void retryClicked(ChatTree::NodeId node);   // signal
// ChatPanel
void onRetry(ChatTree::NodeId reply);
int m_pathLimit = -1;                      // while a retry streams: path messages that count (the user message, not the old reply); -1 otherwise. effectiveMessages()/usage use it when pathCount == -1.
```
`refreshRows()` marks retry available only on the row of `m_tree.lastOnPath()` when it is an assistant node and nothing is busy. `onRetry` → `beginSend(PendingSend{Retry, reply, userText})`; messages sent = path up to and including the user message (no new user text appended); compaction slice likewise. `startSend` for `Retry`: set `m_pathLimit = prefixCount`; the user node is `m_tree.activePath().at(prefixCount - 1)`; commit the pending compaction on it (replacing any snapshot there); `renderTranscript(prefixCount, m_pendingNotes)` then the streaming row. Finish (done / `onDone` fallback / stop), in this order: reset `m_pathLimit = -1`; if text non-empty → `addChild(userNode, "assistant", text)`, attach `m_pendingNotes` to it, `row->setNode`; if empty → `renderTranscript()` so the previous reply is shown, status "Retry failed; kept the previous reply."; then `refreshRows()` and `refreshContextUsage()`.

- [ ] **Step 1: Write failing checks:** "only the latest reply has a retry button"; "retry sends the conversation up to the user message, with no extra user message" ; "retry shows a new reply with 2 / 2 and ‹ returns to the old reply"; "the retry reply replaces the old one on screen while streaming" (`holdChat`); "the usage request during a retry excludes the old reply"; "a retry that returns no text keeps the previous reply and its count" (fixture knob `bool chatEmpty` → stream only `{"done":true}`); "a failed retry request (HTTP error) keeps the previous reply"; "Stop before any text keeps the previous reply; Stop after partial text keeps the partial as a new version"; "retry at the compaction threshold posts only the messages up to the user message to /inference/compact, and the resulting summary is used by the next send"; "continuing after a retry sends the new reply as context"; "retry is unavailable while streaming and with the backend not ready"; "retry with the backend ready but no model loaded (usage 409, chat error) keeps the previous reply"; "clearing the chat while a retry streams leaves nothing behind"; "the draft in the main composer is untouched by retry".
- [ ] **Step 2: Implement** per the Interfaces block.
- [ ] **Step 3: Run the full UI check.** Expected: new checks pass; failure list = baseline.
- [ ] **Step 4: Record progress.**

---

### Task 8: Accessibility, docs and verification

**Files:**
- Modify: `app/src/panels/messagerow.cpp`, `docs/development/architecture.md`, `docs/superpowers/plans/2026-11-06-edit-prompts.md` (progress)

- [ ] **Step 1: Write failing checks:** "pencil, retry, arrows and the editor expose their accessible names" (`accessibleName()` equals "Edit message", "Retry reply", "Previous version", "Next version"; editor box "Edit your message"); "pencil, retry and arrows are focusable by keyboard" (`focusPolicy() != Qt::NoFocus`). Run: expect failures for whatever Task 4–7 left unset.
- [ ] **Step 2: Fix** any gap; add a short "Conversation tree" paragraph to `docs/development/architecture.md` (tree, active path, rows, snapshots on nodes, send flow reuse).
- [ ] **Step 3: Full verification.** Run all three and paste the tails: `cmake --build build -j`; `./build/magnite-app --ui-check 2>&1 | tail -15` (failures = the 4 baseline); `./build/magnite-app --theme-check` ("0 failure(s)"); `PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests 2>&1 | tail -3` (228 OK).
- [ ] **Step 4: Real-model check** (SmolLM2-135M, as in feature 1; use `./build/magnite-app` against a real backend): three-turn chat; edit turn 1 → different continuation, flip to the original and see all three turns intact; retry the last reply → `2 / 2`, flip between the two; edit after an auto-compact (low threshold) and confirm the earlier summary still applies and returns when flipping back. Take `./build/magnite-app --screenshot <dir>` of the edit box and of a switcher row and look at them. Report anything that looks wrong.
- [ ] **Step 5: Record progress** and update `memory/feature-roadmap-2026-11.md` (feature 2 built, uncommitted).
