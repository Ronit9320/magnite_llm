# Chat history (feature 3/5)

## Why

Today the Playground holds exactly one conversation. Closing the app, or pressing "+ New chat", throws it away. People want to come back to an earlier chat, with everything that makes it theirs: edited prompts, retried replies, the `‹ n / total ›` versions, and the compaction summaries that keep long chats inside the model's context. Features 1 and 2 already keep all of that in a plain-data `ChatTree` so that it could be saved as it is. This feature saves it.

## What the user sees

- **A list of chats under "Playground" in the left menu**, newest activity first. It shows about 4–5 rows and scrolls inside itself for the rest. The first row is "+ New chat".
- Clicking a row opens that chat exactly as it was left: same messages, same chosen versions, same compaction dividers, same context meter reading (re-measured against the model that is loaded now).
- The open chat's row is highlighted.
- Right-click on a row (or the keyboard's menu key) offers **Rename** and **Delete**. Delete asks first. Deleting the open chat leaves a blank new chat.
- The old toolbar "+ New chat" button in the Playground is removed. The list's "+ New chat" row replaces it. The context meter's "start a new chat" prompt still works and now starts a new chat while keeping the old one.
- On launch the Playground shows a **blank new chat**; earlier chats are in the list.
- A chat's name is its first message, shortened to one line. Rename overrides it.
- **A friendly greeting on every blank chat**, in the spot where "Your model. Your way." is now (the small "THE MAGNITE WORKSPACE" line and the hint under it stay). It depends on the time of day and uses the user's name, for example "Good morning, Archit", "Good afternoon, Archit", "Good evening, Archit", "Up late, Archit?". Each time a blank chat appears (app launch, "+ New chat", after deleting the open chat) one of a few friendly variants for that time of day is picked at random, never the same one twice in a row. It stays fixed while that blank chat is on screen. Without a name it reads "Good morning" and so on.
- **Asking for the name, once.** The first time the app starts and no name is stored, a small themed dialog asks "What should I call you?" with the computer account name pre-filled as a suggestion, a Save button and a Skip button. Skip is remembered, so it is never asked again. The name can be changed or cleared any time in Settings ("Your name").

## Goals

- Every chat survives an app restart, including all versions, notes and compaction snapshots, with no loss.
- Nothing the user typed is lost by a crash: the chat is saved as soon as their first message is sent, and again after every change (new message, finished reply, edit, retry, version switch, compaction).
- Opening an old chat and continuing it behaves exactly like continuing a live one (edit, retry, compact, switch versions all still work).
- Data stays local, in the app's data folder, in a plain readable format.

## Non-goals (for this feature)

- Search, folders, tags, pinning, export/import of chats.
- Remembering which model or adapter a chat used. An opened chat continues with whatever model is loaded now.
- Syncing between devices or any backend/cloud involvement.
- Encrypting chats. They are stored unencrypted, like the app's other data.
- Grouping rows by date ("Today", "Yesterday").
- Any other personalisation (avatar, profile page). The name is used only for the greeting.
- The name is never sent anywhere: not to the model, not to the backend.

## Anti-goals (would make this a failure even if it technically works)

- A saved chat that reopens subtly different (a version missing, a divider gone, a summary lost).
- Any chance of one bad or half-written file taking down the list or the app.
- Opening or deleting a chat while a reply is streaming and corrupting that reply or the chat.
- Tests or the UI check writing into the user's real `~/.magnite`.

## How it works

- **Storage (my call).** The C++ client writes one JSON file per chat, `<data folder>/chats/<chat id>.json`, using atomic writes (write to a temporary file, then rename) so a crash never leaves half a file. The data folder is resolved the way the backend does: `MAGNITE_HOME` if set, else the Settings "data directory" (`storage/dataDir`), else `~/.magnite`. The backend is not involved: chats must be readable before the backend finishes starting (about 13 seconds), and storing a conversation is not ML.
- **File contents.** `{ "version": 1, "id", "title", "customTitle" (true once renamed), "created", "updated", "tree": … }`. `tree` is `ChatTree` written out node by node: for each node its id, parent, role, content, children in order, active child, notes and compaction `{summary, covered}`, plus the root's children and active child. Ids are preserved so everything that refers to them (active child, snapshots) stays correct.
- **`ChatTree` gets `toJson()` / `fromJson()`.** `fromJson` validates everything (ids in range, every child's parent matches, no cycles, active child is one of the children, known roles) and refuses anything inconsistent rather than building a broken tree.
- **A chat store** (new, plain data plus file access, no widgets, so it can be tested on its own with a temporary folder) lists, loads, saves, renames and deletes chats. The chat panel is handed the store; the UI check gives it a temporary folder.
- **The list** is a new widget in the left menu below the Playground button, driven by the store. It is shown while the Playground is the open workspace and hidden on other workspaces (my call; keeps the menu from crowding, and feature 4 will make the menu collapsible).
- **When saves happen.** A new chat gets an id and a file the moment its first message is sent (an empty chat is never saved). After that, any change to the tree saves, batched by a very short timer so rapid changes (streaming ends, then the view refreshes) are one write, and flushed immediately on switching chats, starting a new chat and closing the app. While a reply is streaming the half-written reply is not saved; the saved chat ends at the user's message until the reply finishes. If the user presses Stop, the partial reply is kept (as today) and saved.
- **Greeting (my calls on the details).** A small pure function takes the hour, the name and a random pick and returns the text, so it can be tested without a clock. Time bands: 05–11 morning, 12–16 afternoon, 17–21 evening, 22–04 late night. About three variants per band, friendly and short, no emoji. The name is stored in `QSettings` as `profile/name`, with `profile/nameAsked` marking that the first-launch dialog has been answered or skipped. A name is trimmed and capped at about 40 characters; blank means no name. The dialog appears once the main window is shown, never during tests or `--selftest`/`--screenshot` runs. The account-name suggestion comes from the operating system user.
- **Opening a chat** replaces the panel's tree with the loaded one and redraws the transcript with the existing `renderTranscript()`. It resets all send state the same way "new chat" does today (that code, `onClearChat`, keeps its name and becomes "save the current chat, then start a blank one").
- **While busy.** Switching, creating, renaming-the-open-chat and deleting are disabled while a reply is streaming, a send is being prepared (measuring/compacting), a model is loading, or a message editor is open, the same guards as the pencils and arrows today. Renaming or deleting a *different*, idle chat is allowed any time except while it is being written.
- **Bad files (my call).** A file that cannot be read or fails validation is skipped in the list and left untouched on disk (never deleted or overwritten automatically). The rest of the list loads normally.
- **List loading (my call).** The list reads every chat file once at launch to get titles and dates, and keeps that in memory. This is fine for hundreds of chats; an index file is a later optimisation if it is ever slow.
- **Format version.** `version: 1`. A file with a newer version than the app understands is treated as a bad file (skipped, left alone). No migration framework.

## Constraints

- C++20/Qt6, existing theme rules: no hard-coded colours (new tokens in `theme.h`, extended `Theme::selfCheck()`), Lucide-style icons tinted with `Theme::coloredIcon()`, new `.cpp` files added to `MAGNITE_SOURCES`.
- No change to any backend route; backend suite stays at 228 tests.
- Keyboard and accessibility like the rest of the app: the list is reachable by keyboard (arrow keys, Enter to open, Delete key / context menu key for the menu), rows have accessible names.
- The chat file format and the `DATA_SCHEMA` contract: chats are a new persistent format. `DATA_SCHEMA` is the update compatibility contract and is bumped only when persistent formats change in a way an update could break. Whether this new, versioned, additive folder needs a bump is for the builder to decide against `docs/development/RELEASE_PIPELINE.md`; the default is no bump because older app versions simply ignore the folder.

## Left to the builder

- The exact look of rows (height, truncation, hover) following existing sidebar/nav styling and the theme rules, checked with screenshots in light and dark.
- How the list and the chat panel talk to each other (signals) and how the store is injected in tests.
- Row height so that about 4–5 rows are visible, and that the menu still fits at the app's minimum window height.
- Title shortening rule (one line, trimmed, about 40 characters with an ellipsis, blank messages fall back to "New chat").

## Verification

- Unit-style checks on `ChatTree` JSON round-trip (versions, active choices, notes, compaction, empty tree) and on rejecting corrupt trees.
- Store checks with a temporary folder: create/save/list/load/rename/delete, atomic write, bad file skipped, newer-version file skipped, ordering by last activity.
- Chat-panel checks: first message creates a file, a reply saves, edit/retry/switch save, reopening restores the exact transcript and chosen versions and compaction dividers, switching while streaming is blocked, deleting the open chat leaves a blank chat.
- Greeting checks: right wording and name for each time band including the band edges, no-name wording, no repeat twice in a row, stays fixed while the blank chat is shown, changes when a new blank chat appears; first-launch dialog asked once, Skip remembered, Settings "Your name" changes and clears it.
- List widget checks: shows newest first, scrolls after ~5 rows, highlights the open chat, rename/delete flows, keyboard.
- Full suites: `--ui-check` stays at its 4 known failures with the new checks passing, `--theme-check` 0 failures, backend 228 OK.
