# Chat History (feature 3/5) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (the user chose in-session native execution) to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Save every chat automatically, list the saved chats under "Playground" in the left menu (scrolling after ~5), reopen them exactly as left, and greet the user by name and time of day on every blank chat.

**Architecture:** Client-only. `ChatTree` learns to write/read itself as JSON. A new `ChatStore` keeps one JSON file per chat under `<data folder>/chats/` with atomic writes. `ChatPanel` is handed a store (none = no saving, so every existing check is unaffected), saves after every tree change, and can open/start/rename/delete chats. A new `ChatHistoryList` widget in the main window's left menu shows the store's list. A small `Greeting` module (pure) and a first-launch `NameDialog` supply the greeting.

**Tech Stack:** C++20 / Qt6 Widgets, QSettings, QJson, QSaveFile, the loopback fixture and `check(...)` harness in `app/src/testing/uicheck.cpp` (`./build/magnite-app --ui-check`).

**Spec:** `docs/superpowers/specs/2026-11-06-chat-history-design.md`

## Global Constraints

- No hex colours in widget code or QSS (AGENTS.md). Existing tokens (`@surfaceFill`, `@surfaceFillHover`, `@surfaceFillActive`, `@surfaceBorder`, `@textPrimary`, `@textSecondary`, `@textMuted`, `@accentPrimary`) should be enough; if a new colour is needed add a token in `app/src/theme.h` and extend `Theme::selfCheck()`. Reuse the existing `plus`, `trash`, `pencil` icons (already in `kIcons`); a new icon must go in `app/resources.qrc` and `kIcons` in `app/src/theme.cpp`.
- New C++ files go in `MAGNITE_SOURCES` in `app/CMakeLists.txt` (explicit list).
- Chat file: `<data folder>/chats/<id>.json`, `{ "version": 1, "id", "title", "customTitle", "created", "updated", "tree" }`. Data folder = `MAGNITE_HOME` if set, else `QSettings` `storage/dataDir` if non-empty, else `~/.magnite` (same order as `backend_manager.cpp:93-94`).
- Chat id = `QUuid::createUuid().toString(QUuid::WithoutBraces)`; any id that does not match `^[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}$` is rejected by every store call (no path tricks).
- Titles: first message of the tree's first root child, whitespace collapsed to single spaces, cut to 40 characters + `…`, blank falls back to `New chat`. A renamed chat (`customTitle`) keeps its name on every later save.
- Greeting time bands (local hour): 5–11 morning, 12–16 afternoon, 17–21 evening, 22–4 late night. Name is trimmed, capped at 40 characters; blank = no name. Settings keys: `profile/name`, `profile/nameAsked`.
- The toolbar "+ New chat" button in the Playground is removed. `ChatPanel::onClearChat()` keeps its name and (unguarded) behaviour: stop whatever is running, save the current chat, show a blank one. Existing checks rely on this.
- Switching chats, "+ New chat" in the list, and deleting the open chat are disabled while a reply streams, a send is prepared (`m_preparing`), a model loads, or an editor is open. A half-streamed reply is never saved (it is not in the tree until `commitReply`).
- The name is never sent to the backend or model. No backend change; backend suite stays at 228 tests OK: `PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests`.
- Checks and tests must never touch the real `~/.magnite` or real `QSettings`: stores are created on a `QTemporaryDir`; `--ui-check` already redirects `QSettings` to a temp folder (`main.cpp`).
- `Theme::minWindowHeight()` stays 680. The history list gives way (shrinks) rather than the window growing.
- Local only: **no commits and no push until the final step**; the single commit is `feat(chat): feature 3/5 - chat history` with **no Co-Authored-By line and no Claude footer** (standing user instruction, overrides any default attribution). Every task ends with "Record progress": tick that task's checkboxes in this file.
- Baseline: `./build/magnite-app --ui-check` = 411 ok and exactly 4 failures ("training/settings dropdown supports keyboard selection", two each); the first run after a rebuild may show 1–3 extra timing failures, re-run before investigating. `--theme-check` = 0 failures. Build: `cmake --build build -j`. After restoring files from a backup, `touch` the sources first.

## Review Focus

1. A chat file that is corrupt, truncated, empty, from a newer app version, has a wrong id, or a tree with broken links — skipped in the list, left untouched on disk, the rest of the list and the app work. (Tasks 1, 2, 4)
2. Quit/crash timing: a chat exists on disk after the first message is sent, and a reply still streaming is not half-saved; stopping a stream keeps the partial reply and saves it. (Task 4)
3. Deleting the open chat must not resurrect it by a later flush/save; deleting or switching while busy does nothing. (Tasks 4, 6)
4. Reopening restores *exactly* — non-active versions, edit/retry branches, dividers, compaction `{summary, covered}` — and the model request after reopening matches the one before saving. (Tasks 1, 4)
5. Names with unusual text (empty, whitespace, very long, non-ASCII, quotes) for chat titles and the user name; greeting at the hour edges (4→5, 11→12, 16→17, 21→22, 0). (Tasks 2, 3, 5)

---

## File Structure

| File | Responsibility |
|---|---|
| `app/src/panels/chattree.{h,cpp}` (modify) | `toJson()` / `fromJson()` with full validation. |
| `app/src/panels/chatstore.{h,cpp}` (new) | `ChatStore`: files, listing, atomic save, load, rename, delete, titles. No widgets. |
| `app/src/panels/greeting.{h,cpp}` (new) | `Greeting`: pure wording, name/asked settings helpers. |
| `app/src/widgets/namedialog.{h,cpp}` (new) | First-launch "What should I call you?" dialog. |
| `app/src/widgets/chathistorylist.{h,cpp}` (new) | The list under Playground: "+ New chat" row, scrolling list, context menu, keyboard. |
| `app/src/panels/chathistorycontroller.{h,cpp}` (new) | Connects store, panel and list; rename/delete dialogs behind replaceable seams. |
| `app/src/panels/chatpanel.{h,cpp}` (modify) | Store injection, autosave, open/new/rename/delete, greeting, toolbar button removed. |
| `app/src/panels/settingspanel.{h,cpp}` (modify) | "Your name" field in the Chat tab. |
| `app/src/mainwindow.{h,cpp}` (modify) | Creates the store, places the list, wires dialogs, first-launch name prompt. |
| `app/resources/theme/console.qss` (modify) | Styles for the list and dialog, tokens only. |
| `app/src/testing/uicheck.cpp` (modify) | All new checks. |
| `app/CMakeLists.txt` (modify) | Add new sources. |
| `docs/development/architecture.md`, `docs/superpowers/` (modify) | Short "Chat history" section; tick this plan. |

---

### Task 1: `ChatTree` JSON round-trip

**Files:**
- Modify: `app/src/panels/chattree.h`, `app/src/panels/chattree.cpp`, `app/src/testing/uicheck.cpp` (next to the existing `ChatTree` checks)

**Interfaces:**
- Produces: `QJsonObject ChatTree::toJson() const;` and `static std::optional<ChatTree> ChatTree::fromJson(const QJsonObject &json);` (nullopt for anything invalid). Format: `{"nodes":[{"id","parent","role","content","children":[ids],"activeChild","notes":[strings],"compaction":{"summary","covered"}}],"rootChildren":[ids],"rootActive":id}` — nodes in id order, ids preserved.

- [x] **Step 1: Write failing checks.** (a) A tree with two root versions, an edited user message, a retried reply, two notes and a compaction on different nodes: `ChatTree::fromJson(t.toJson())` has identical `activePath()`, `versionsOf`, `versionCount`, `notes`, `compaction()`, `messages()`, and after `activate()` of the non-active branch both trees agree again. (b) Empty tree round-trips and is `isEmpty()`. (c) `fromJson` returns nullopt for each of: node `parent` out of range, a child whose `parent` differs, a child listed twice, a cycle (node is its own ancestor), `activeChild` not among `children`, `rootActive` not among `rootChildren` (when `rootChildren` non-empty), unknown role (anything but `user`/`assistant`/`system`), `covered` negative, missing `nodes`, wrong types. (d) The model request is unchanged: `messages()` and `compaction()` of the reloaded tree equal the original for a compacted conversation.
- [x] **Step 2: Run** `cmake --build build -j && ./build/magnite-app --ui-check 2>&1 | grep -E "FAIL|ChatTree"` — expect the new checks to fail to compile/fail.
- [x] **Step 3: Implement** both functions in `chattree.cpp`. `fromJson` rebuilds `m_nodes` directly (friend access or a private constructor path) after validating every rule in Step 1 before it keeps anything; unreachable nodes (not under `rootChildren`) are invalid.
- [x] **Step 4: Run** the same command; new checks `ok`, failure list identical to baseline.
- [x] **Step 5: Record progress.**

### Task 2: `ChatStore`

**Files:**
- Create: `app/src/panels/chatstore.h`, `app/src/panels/chatstore.cpp`
- Modify: `app/CMakeLists.txt`, `app/src/testing/uicheck.cpp`

**Interfaces:**
- Consumes: `ChatTree::toJson/fromJson`.
- Produces:
  ```cpp
  struct ChatSummary { QString id, title; bool customTitle = false; QDateTime created, updated; };
  class ChatStore : public QObject {
  public:
      explicit ChatStore(const QString &chatsDir, QObject *parent = nullptr);   // creates the dir lazily on first save
      static QString defaultDir();                         // data folder rule from Global Constraints + "/chats"
      QList<ChatSummary> list() const;                     // newest `updated` first; cached, built at construction
      QString create();                                    // new valid id; writes nothing
      bool save(const QString &id, const ChatTree &tree);  // atomic (QSaveFile); keeps created + custom title; false + lastError() on failure
      std::optional<ChatTree> load(const QString &id) const;   // nullopt for missing/corrupt/newer-version/invalid id
      bool rename(const QString &id, const QString &title);    // trimmed, non-empty; sets customTitle
      bool remove(const QString &id);
      QString lastError() const;
      static QString titleFor(const ChatTree &tree);       // rule in Global Constraints
      static bool validId(const QString &id);
  signals: void changed();                                 // after every successful save/rename/remove
  };
  ```

- [x] **Step 1: Write failing checks** on a `QTemporaryDir`: create→save→`list()` has one row with title from the first message; a second chat saved later is listed first; `load` returns an equal tree; rename sets title and survives a further `save` of the same id; `remove` deletes the file and the row; `changed` fires once per mutation; `validId` accepts a real uuid and rejects `""`, `"../x"`, `"a/b"`, uppercase, wrong length; `titleFor` for empty tree → `New chat`, whitespace-only first message → `New chat`, 100-char message → 40 chars + `…`, text with newlines/tabs collapses to single spaces, non-ASCII is not split mid-character; a new `ChatStore` on the same folder lists the same rows (persistence); bad files: write `garbage.json`, a `0-byte` uuid file, a valid-JSON file with `version: 99`, a file whose `id` field differs from its filename, and a file with a broken tree — none appear in `list()`, `load` of each returns nullopt, and every file is still on disk afterwards; no `*.tmp`/partial files remain after a save; `save` into an unwritable path returns false with `lastError()` non-empty and the previous good file intact.
- [x] **Step 2: Run** the build + `--ui-check`; expect failures.
- [x] **Step 3: Implement** in `chatstore.cpp`. Read every `*.json` once in the constructor to fill the in-memory list (skip invalid per Step 1); use `QSaveFile` with `commit()`; `updated` = now on every save/rename; ISO-8601 strings in the file.
- [x] **Step 4: Run**; new checks `ok`, baseline failures only.
- [x] **Step 5: Record progress.**

### Task 3: `Greeting` (pure wording + name settings)

**Files:**
- Create: `app/src/panels/greeting.h`, `app/src/panels/greeting.cpp`
- Modify: `app/CMakeLists.txt`, `app/src/testing/uicheck.cpp`

**Interfaces:**
- Produces (namespace `Greeting`):
  ```cpp
  QStringList variants(int hour, const QString &name);          // all wordings for that hour's band, name applied
  QString pick(const QStringList &options, const QString &previous, int random);   // options[random % n], never == previous when n > 1
  QString userName();                  QString cleanName(const QString &raw);      // QSettings profile/name; trim + cap 40
  void setUserName(const QString &name);
  bool nameAsked();                    void setNameAsked(bool asked);
  QString accountNameSuggestion();     // $USER / $USERNAME with first letter upper-cased, "" if unknown
  ```
- Wordings (exact; `{n}` = name; without a name the `, {n}` / ` {n}` part is dropped, keeping the punctuation shown after it):

  | Band | Variants (with name → without) |
  |---|---|
  | morning | "Good morning, {n}" → "Good morning"; "Morning, {n}" → "Morning"; "Rise and shine, {n}" → "Rise and shine" |
  | afternoon | "Good afternoon, {n}" → "Good afternoon"; "Afternoon, {n}" → "Afternoon"; "Hope your day is going well, {n}" → "Hope your day is going well" |
  | evening | "Good evening, {n}" → "Good evening"; "Evening, {n}" → "Evening"; "Winding down, {n}?" → "Winding down?" |
  | late night | "Up late, {n}?" → "Up late?"; "Burning the midnight oil, {n}?" → "Burning the midnight oil?"; "Still up, {n}?" → "Still up?" |

- [x] **Step 1: Write failing checks:** `variants(5,"Archit")` first element is `Good morning, Archit`; each band edge hour (0→late, 4→late, 5→morning, 11→morning, 12→afternoon, 16→afternoon, 17→evening, 21→evening, 22→late, 23→late) returns that band's three strings; empty/whitespace name gives the no-name strings; `Up late, Archit?` and `Up late?` appear for hour 23; `pick` never returns `previous` over every `random` in 0..20 when `options.size()>1`, returns the only option when size 1, `random` huge/negative is safe; `cleanName("  Ada  ")=="Ada"`, 60-char input capped at 40, whitespace-only → `""`; `setUserName`/`userName`/`nameAsked` round-trip through `QSettings` (cleaned up after); `accountNameSuggestion()` is empty or starts upper-case.
- [x] **Step 2: Run**; expect failures.
- [x] **Step 3: Implement** `greeting.cpp` per the table; account name from `qEnvironmentVariable("USER")`, falling back to `"USERNAME"`.
- [x] **Step 4: Run**; new checks `ok`, baseline failures only.
- [x] **Step 5: Record progress.**

### Task 4: `ChatPanel` saving and opening

**Files:**
- Modify: `app/src/panels/chatpanel.h`, `app/src/panels/chatpanel.cpp`, `app/src/testing/uicheck.cpp`

**Interfaces:**
- Consumes: `ChatStore`, `ChatTree`.
- Produces:
  ```cpp
  void setChatStore(ChatStore *store);          // null = no saving (default; keeps all existing checks unchanged)
  QString currentChatId() const;                // "" for a blank, unsaved chat
  void flushSave();                             // writes any pending change now (no-op without a store or when nothing is pending)
  bool isBusy() const;                          // streaming || preparing || loading || anyEditing()
  bool openChat(const QString &id);             // false (nothing changes) when busy, unknown or unreadable
  bool renameChat(const QString &id, const QString &title);   // false when empty title, unknown id, or id is the open chat while busy
  bool deleteChat(const QString &id);           // false when it is the open chat and busy, or unknown
  signals: void currentChatChanged(const QString &id);   void busyChanged(bool busy);
  ```
  `onClearChat()` stays a slot: stops streaming/preparing, flushes the save, shows a blank chat (id cleared, `currentChatChanged("")`).

- [x] **Step 1: Write failing checks** (panel with `setChatStore` on a `QTemporaryDir` store and the existing fixture): (a) a blank chat writes no file; sending the first message creates exactly one file immediately (before the reply arrives), whose reloaded tree ends at the user message; after the reply, the file holds both; (b) edit → new version saved; retry → saved; version switch → saved with the new active choice; a compaction (reuse the existing compaction fixture knobs) → snapshot and divider note in the file; (c) `openChat` on a saved chat after `onClearChat` shows the same row texts, the same `‹ n / total ›` values, same dividers, and the next request body equals what the original chat would have sent (compare `fixture.chatRequests.last()` messages) — repeat after switching to an older version and after a compaction; (d) opening while streaming / preparing / loading / editing returns false and changes nothing; (e) stopping a stream saves the partial reply; (f) `deleteChat(open)` removes the file, shows a blank chat, and a later `onClearChat`/send of a *new* message does not recreate the deleted id; `deleteChat(other)` leaves the open chat alone; deleting the open chat while streaming returns false; (g) `renameChat` changes `list()` title and survives further messages; empty title → false; (h) bad file in the folder: `openChat(thatId)` false with a status message and the current chat untouched; (i) `store==nullptr`: a full send/edit/retry cycle writes nothing and nothing crashes; (j) the toolbar has no "+  New chat" button (`button(&chat,"+  New chat")==nullptr`) and the meter's new-chat prompt still starts a new chat while keeping the old one in the store; (k) the composer draft survives `openChat`; (l) a failed save (read-only folder) shows a `warn` status and the conversation continues.
- [x] **Step 2: Run**; expect failures.
- [x] **Step 3: Implement.** Add `markDirty()` (single-shot ~150 ms timer; creates the chat id via `store->create()` on first non-empty save, and that very first save is written immediately rather than batched so the file exists the moment the first message is sent; no-op for an empty tree or null store) and `flushSave()`. Call `markDirty()` at **every** call site of `m_tree.addChild`, `activate`, `addNote` and `setCompaction` (grep for them), `flushSave()` at the start of `openChat` and in `onClearChat`; `MainWindow`'s destructor calls it too (Task 7). `ChatPanel`'s own destructor must not touch the store (the window owns it). Extract the state-reset part of `onClearChat` into a private `resetConversation()` shared with `openChat`; `openChat` then installs the loaded tree, `renderTranscript()`, `scrollToBottom()`, `updateActions()`, `refreshContextUsage()`, emits `currentChatChanged`. Delete `m_clearButton` and its toolbar entry (keep `onClearChat` connected to the meter). Emit `busyChanged` from `updateActions()` when `isBusy()` changes. After `deleteChat(open)` clear the id before any flush so nothing is resaved.
- [x] **Step 4: Run**; new checks `ok`; baseline failures only (existing chat checks that referenced the toolbar button, if any, adjusted to `onClearChat`).
- [x] **Step 5: Record progress.**

### Task 5: Greeting on blank chats, name dialog, "Your name" setting

**Files:**
- Create: `app/src/widgets/namedialog.h`, `app/src/widgets/namedialog.cpp`
- Modify: `app/CMakeLists.txt`, `app/src/panels/chatpanel.{h,cpp}`, `app/src/panels/settingspanel.{h,cpp}`, `app/resources/theme/console.qss` (dialog styling only if needed), `app/src/testing/uicheck.cpp`

**Interfaces:**
- Consumes: `Greeting::*`.
- Produces: `class NameDialog : public QDialog { explicit NameDialog(const QString &suggestion, QWidget *parent = nullptr); QString name() const; }` with a `QLineEdit` (`objectName "nameInput"`, accessible name "Your name", pre-filled with the suggestion), a `Save` button (`primaryBtn`, default) and a `Skip` button (`secondaryBtn`); heading "What should I call you?". `Save` accepts, `Skip`/Esc rejects. `void ChatPanel::setHourProviderForTest(std::function<int()>)` (default `QTime::currentTime().hour()`). Settings: a `QLineEdit` (`objectName "profileName"`, accessible name "Your name") in the Chat tab, applied on `editingFinished`, writes via `Greeting::setUserName`, emits `settingsChanged`.

- [x] **Step 1: Write failing checks:** (a) the blank chat's `emptyConversation` label shows one of `Greeting::variants(hour,name)` for the injected hour and name, and does not contain "Your model. Your way."; (b) calling `onClearChat` repeatedly never shows the same text twice in a row (loop 12×) and the text stays unchanged across `redrawForTest()`, theme toggle and a window resize; (c) opening a non-empty chat hides the welcome; returning to blank shows a greeting again; (d) changing the name through `Settings` (set `profileName`, `editingFinished`) re-greets the blank chat with the new name and clearing it drops the name; (e) `NameDialog`: pre-filled with the suggestion; `Save` → `Accepted` and `name()` trimmed; `Skip` → `Rejected`; a 60-char entry is capped at 40 by `Greeting::cleanName`; (f) the "What should I call you?" heading and both buttons exist with accessible names.
- [x] **Step 2: Run**; expect failures.
- [x] **Step 3: Implement.** In `chatpanel.cpp` replace the fixed "Your model. Your way." with a private `showBlankChat()` that picks via `Greeting::pick(Greeting::variants(hour,name), m_greeting, QRandomGenerator::global()->generate())`, stores `m_greeting`, sets the label, and is called from `setupUi()`, `resetConversation()`; `reloadContextSettings()` also re-greets when the chat is blank and the name changed. `NameDialog` follows `confirmation.h` style (plain `QDialog`, existing object names so the global QSS applies). Settings field goes in the existing Chat tab card.
- [x] **Step 4: Run**; new checks `ok`; fix `mainwindow.cpp:554` (`emptyChat` screenshot helper) if it relied on the old text; baseline failures only.
- [x] **Step 5: Record progress.**

### Task 6: `ChatHistoryList` widget

**Files:**
- Create: `app/src/widgets/chathistorylist.h`, `app/src/widgets/chathistorylist.cpp`
- Modify: `app/CMakeLists.txt`, `app/resources/theme/console.qss`, `app/src/testing/uicheck.cpp`

**Interfaces:**
- Consumes: `ChatSummary`.
- Produces:
  ```cpp
  class ChatHistoryList : public QWidget {
  public:
      explicit ChatHistoryList(QWidget *parent = nullptr);
      void setChats(const QList<ChatSummary> &chats, const QString &openId);   // keeps scroll position and keyboard focus where it can
      void setBusy(bool busy);       // disables "+ New chat", opening other chats and actions on the open chat
      static constexpr int kVisibleRows = 5;
  signals:
      void newChatRequested();   void openRequested(const QString &id);
      void renameRequested(const QString &id);   void deleteRequested(const QString &id);
  };
  ```
  Children: a `QPushButton` "+ New chat" (`objectName "historyNew"`, `plus` icon) and a `QListWidget` (`objectName "historyList"`, accessible name "Saved chats"). Row height fixed (e.g. 30 px); the list's maximum height is `kVisibleRows` rows, minimum two rows; vertical scroll bar when there are more; elided single-line titles with the full title as tooltip. Context menu "Rename" / "Delete"; the Delete key and Enter/Return work on the focused row; the open chat's row is selected/highlighted.

- [x] **Step 1: Write failing checks:** rows appear in the order given; the open chat's row is the current item; with 12 chats the list's visible height shows 5 rows and has a vertical scroll bar, with 3 chats no scroll bar and the widget shrinks; clicking or Enter on a row emits `openRequested(id)` once; clicking "+ New chat" emits `newChatRequested`; Delete key emits `deleteRequested` for the focused row; the context menu has exactly "Rename" and "Delete" and emits the right signals (trigger the actions directly); `setBusy(true)` makes the button disabled, other rows non-activating and the open row's actions disabled, other rows' Rename/Delete still enabled; a title of 200 characters is elided and its tooltip is the full text; `setChats` with an empty list shows only the button; calling `setChats` again keeps the scroll value when the chat set is unchanged; accessible names set; light and dark screenshots (when `--ui-check <dir>` is given) via the existing `capture`.
- [x] **Step 2: Run**; expect failures.
- [x] **Step 3: Implement** with theme-token QSS (`QListWidget#historyList`, `QPushButton#historyNew`), no hex.
- [x] **Step 4: Run**; new checks `ok`; `--theme-check` still 0 failures; baseline failures only.
- [x] **Step 5: Record progress.**

### Task 7: Chat history controller and main-window wiring

`--ui-check` cannot construct a `MainWindow` (it starts the backend process), so the wiring between the store, the panel, the list and the dialogs lives in a small controller that the check can build on its own; `MainWindow` only creates it.

**Files:**
- Create: `app/src/panels/chathistorycontroller.h`, `app/src/panels/chathistorycontroller.cpp`
- Modify: `app/CMakeLists.txt`, `app/src/mainwindow.h`, `app/src/mainwindow.cpp`, `app/src/testing/uicheck.cpp`

**Interfaces:**
- Consumes: `ChatStore`, `ChatPanel`, `ChatHistoryList`.
- Produces:
  ```cpp
  class ChatHistoryController : public QObject {
  public:
      ChatHistoryController(ChatStore *store, ChatPanel *panel, ChatHistoryList *list, QWidget *dialogParent);
      // Dialog seams, replaced in checks. Defaults: a themed QInputDialog ("Rename chat", current title pre-filled; empty = cancelled)
      // and Ui::confirmRemoval(parent, "Delete chat?", "This chat and all its versions will be removed.", "Delete").
      std::function<QString(const QString &currentTitle)> askTitle;
      std::function<bool(const QString &title)> confirmDelete;
  };
  ```
  In `MainWindow`: `ChatStore *m_chatStore`, `ChatHistoryList *m_history`, `ChatHistoryController *m_historyController`, private `void askForNameIfNeeded()`.

- [x] **Step 1: Write failing checks** (panel + store on a `QTemporaryDir` + a `ChatHistoryList` + the controller with stubbed `askTitle`/`confirmDelete`): the list shows exactly `store->list()` and refreshes on `ChatStore::changed`; the open chat's row is highlighted and follows `currentChatChanged`; `busyChanged` reaches `setBusy`; `openRequested(id)` opens that chat; `newChatRequested` starts a blank chat and keeps the old one listed; `renameRequested` with a stubbed title renames, an empty answer changes nothing; `deleteRequested` with `confirmDelete` returning false changes nothing, returning true deletes (and, for the open chat, shows a blank chat); the stub receives the chat's title.
- [x] **Step 2: Run**; expect failures.
- [x] **Step 3: Implement** the controller, then in `MainWindow::setupUi()` insert `m_history` into the `navigation` layout straight after the Playground button (before `addStretch`), with the spare height (stretch factor 1, expanding up to five rows); `switchPanel` shows it only while the Playground is the open workspace. Create `m_chatStore` and call `m_chatPanel->setChatStore(...)`: on `ChatStore::defaultDir()` for a normal run, on a `QTemporaryDir` owned by the window for `--selftest` and `--screenshot` runs so they never write to the real folder. `~MainWindow` calls the panel's `flushSave()` first, before `savePanelSettings()`. `askForNameIfNeeded()` runs once via `QTimer::singleShot(0)` after `show()` when `!Greeting::nameAsked()` and not in a verification run (`--selftest`, `--screenshot`); it shows `NameDialog(Greeting::accountNameSuggestion())`, stores the cleaned name on Save, and sets `nameAsked` on both Save and Skip. If the sidebar does not fit at 680 px, reduce the list's minimum first; do not raise `minWindowHeight`.
- [x] **Step 4: Run** the checks. Then `MAGNITE_SCREENSHOT_COMPACT=1 ./build/magnite-app --screenshot <temp dir>` and look at the Playground image: list under Playground, "+ New chat" row, greeting visible, nothing clipped at 1024×680; add several chats first (temporary store) to see the scroll bar. Confirm by running the app that the list is hidden on the other workspaces. Baseline failures only.
- [x] **Step 5: Record progress.**

### Task 8: Docs, full verification, commit

**Files:**
- Modify: `docs/development/architecture.md` (add a "Chat history" subsection after "Conversation tree, editing and retry": storage location and file shape, save triggers, bad-file policy, greeting/name settings), `docs/superpowers/` memory note (tick this plan), `README.md` only if it mentions the Clear button or one-chat limitation.

- [x] **Step 1:** Update the docs; remove the stale sentence in architecture.md that says a future chat-history feature "needs to store" the snapshot.
- [x] **Step 1b: `DATA_SCHEMA` decision.** Read `docs/development/RELEASE_PIPELINE.md` and the `DATA_SCHEMA` notes in `backend/src/backend/_version.py`; chats are an additive, versioned folder that older app versions ignore, so the default is **no bump**. If the pipeline says otherwise, stop and ask before changing it.
- [x] **Step 2: Full suites.** `cmake --build build -j`, then `./build/magnite-app --ui-check` (twice if the first run shows timing flakes) — expect the baseline's 4 failures and nothing else, new total higher than 411; `./build/magnite-app --theme-check` → `0 failure(s)`; `PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests -v` → 228 OK. Confirm `git status` shows nothing under `~/.magnite`-style paths and no stray files in the repo (screenshots go to the temp dir).
- [x] **Step 3: Manual run** (`./build/magnite-app` with `MAGNITE_HOME` set to a temp folder): name prompt appears once; send a message, quit, relaunch → chat is listed and reopens; rename, delete, scroll with 8+ chats; blank chat shows a greeting.
- [x] **Step 4: One fresh reviewer** (most capable model) reads the whole change against the spec and Review Focus; fix what they confirm.
- [x] **Step 5: Commit locally on `feature`:** after checking `git status` shows only this feature's files, `git add -A && git commit -m "feat(chat): feature 3/5 - chat history"` — no Co-Authored-By, no footer, no push. Update the roadmap memory note.
