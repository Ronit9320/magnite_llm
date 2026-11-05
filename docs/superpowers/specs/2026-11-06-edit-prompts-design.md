# Editing sent prompts, retry, and version switching — design

Date: 2026-11-06 · Feature 2 of 5 (context window ✔, **editing prompts**, chat history, sidebar collapse, training charts). Each is designed and built on its own.

## Intent

In Claude.ai and ChatGPT, a message you already sent is not final. You can edit it and the model answers the new version; you can ask for a fresh answer to the same message; and the earlier versions are not lost, you can flip back to them with a small `‹ 1 / 2 ›` switcher. Magnite's chat is a straight line today: once a message is sent the only way to change course is to clear the whole chat.

This feature adds those three things to the chat panel, matching the behaviour people already know from Claude:

1. **Edit** any message you sent, then re-run the conversation from that point.
2. **Retry** the model's latest reply (a new answer to the same message).
3. **Switch** between the versions of an edited message or a retried reply, each with its own continuation.

## Goals

1. **Edit.** Hovering a user message shows a small pencil beside its bubble. Clicking it turns the bubble into an inline text box with **Cancel** and **Save & send**. `Esc` cancels; `Cmd/Ctrl+Enter` saves. Applies to every user message, not only the last.
2. **Save & send** creates a new version of that message. Everything after the old version is hidden from view (it stays reachable through the switcher), and the model answers the new text exactly as if it had been typed at that point, including the context meter check and auto-compact from feature 1.
3. **Retry.** The model's latest reply shows a retry button (existing `refresh-cw` icon) on hover. It streams a new reply to the same user message and adds it as a new version of that reply.
4. **Version switcher.** Any message that has more than one version shows `‹ 2 / 2 ›` right-aligned under a user message, left-aligned under an assistant reply, always visible (not only on hover). Clicking an arrow shows that version and its whole continuation (later messages and replies included). Arrows are disabled at the ends.
5. Editing inside an older version is allowed and branches again from there. The data model is a tree; the screen always shows one path through it.
6. Nothing visible or stored changes until the new send actually begins. Cancelling the edit, cancelling a compaction, or a retry that fails with no text leaves the conversation exactly as it was.
7. The context meter, the compaction summary and the conversation sent to the model always match the path currently on screen.
8. Pencils, retry and switcher arrows are disabled while a reply is streaming, a send is being prepared (measuring/compacting), or a model is loading. Stop remains available as today.
9. A message that never got a reply (for example no model was loaded) can be sent again from its editor: Save & send accepts unchanged text for it and sends the message once, without adding a second copy or a new version.

## Non-goals

- Saving versions across restarts. The tree lives in memory like the chat does today. Feature 3 (chat history) will persist it, so the structure below is plain data (strings and indices) and compaction state is stored on the tree, not in widgets.
- Retrying any reply other than the latest. Earlier replies are changed by editing the message above them. (My call; see below.)
- Editing assistant replies or editing system messages.
- A system prompt (separate idea, not requested).
- Special handling for editing or retrying with no model loaded. Save & send and Retry follow the same rules as typing a new message: if the backend is not connected nothing happens (status error, editor stays open); if it is connected but no model is loaded, the send starts and the backend's error is shown as a generation error, exactly as the composer does today. A failed retry keeps the previous reply; a failed edit leaves the new (reply-less) version next to the original one, reachable through the switcher.
- Any change to the backend. All routes already take the full message list; this is client-only.

## Anti-goals (failure even if it "works")

- Losing a version: any edit, retry, stop, error, or cancelled compaction that makes an earlier version unreachable.
- Showing one thing on screen and sending another to the model (the meter or compaction using a different path than the transcript).
- A retry or edit that fails and leaves the screen showing a half-state (old reply deleted, no new one).
- The switcher showing the wrong count or jumping versions when other branches change.
- Compaction dividers or notes vanishing when you switch away and back.
- Blocking the UI thread.

## Constraints and facts from the codebase

- `ChatPanel` (`app/src/panels/chatpanel.cpp/.h`) keeps a flat `QList<QJsonObject> m_history` of `{role, content}` and separately a layout `m_chatLayout` of `ChatBubble` widgets and divider labels. Bubbles know only their role and text; nothing links a bubble to its history index. `onClearChat` removes layout items and clears `m_history`.
- Feature 1 added `m_compaction {summary, covered}` where `covered` counts leading messages of `m_history` replaced by the summary when sending (`effectiveMessages()`). The transcript is never trimmed. `compactThenSend` builds its own slice of `m_history` and posts it to `/inference/compact`. Dividers ("Context compacted…", "Could not compact…") are layout labels added by `addContextDivider`.
- The send flow is `onSend → measureThenSend / compactThenSend → startSend(text)`. `startSend` creates the user and assistant bubbles, appends the user message to `m_history` and starts the stream; on finish or Stop the assistant text is appended to `m_history` (only if non-empty).
- `ChatBubble` (`app/src/panels/chatbubble.h/.cpp`) is a fixed-size frame with a measured label. It already has a "Copy message" context menu. QSS ids: `chatBubbleUser`, `chatBubbleAssistant` in `app/resources/theme/console.qss`.
- Icons are Lucide-style SVGs in `app/resources/icons/` tinted with `Theme::coloredIcon()`; `refresh-cw.svg` exists. `pencil`, `chevron-left`, `chevron-right` do not yet exist and must be added (and to the resource list). No hard-coded hex values anywhere; new colours are tokens in `theme.h` and covered by `Theme::selfCheck()`. New C++ files go in `MAGNITE_SOURCES` in `app/CMakeLists.txt`.
- Baseline: backend suite 228 tests; `--ui-check` has 4 pre-existing failures ("training/settings dropdown supports keyboard selection") that this work neither fixes nor adds to.

## Design

### Data model: a tree of messages, one active path

Each message is a node: `{role, content, children[], activeChild, notes, compaction?}`. A virtual root holds the first user message(s). The **active path** is root → follow `activeChild` at every node. The tree replaces `m_history` and `m_compaction`; the active path is read back as the same flat `{role, content}` list the rest of the panel already uses, so `effectiveMessages()`, the meter and compaction keep working on a flat list.

- **Versions of a user message** = the siblings under the same parent (the previous assistant reply, or the root for the first message).
- **Versions of an assistant reply** = the siblings under the same user message.
- **Edit** adds a new user node as a sibling of the edited one and makes it active (its children start empty). **Retry** adds a new assistant node as a sibling of the latest reply and makes it active.
- **Switching** sets `activeChild` on the parent, then rebuilds the flat list and the transcript below that point.
- **Notes**: things the transcript shows that are not messages (the compaction divider and the "could not compact" notice) are stored as text on the node they appear *before*, so they are shown again whenever that node is on the active path.
- **Compaction state** `{summary, covered}` is stored on the node whose send produced it. The state in effect is the one on the *last node of the active path that carries one*. An edit or switch away from the node that holds a compaction takes that node off the path, so the earlier compaction (or none) applies automatically, and switching back restores it. No separate invalidation rule is needed. `covered` is an index into the active path at that moment, which stays valid because that node's ancestors are fixed.

### Rendering

The transcript is a drawing of the active path. When the path changes (edit saved, retry started, version switched, chat cleared) the transcript is rebuilt from the path (rows above the changed point may be reused). Each user and assistant bubble carries its node reference so pencil/retry/switcher clicks act on the right node. The switcher is a small widget (two `chevron` buttons and an `n / total` label) shown only when `siblings > 1`.

### Edit flow

1. Pencil click → that bubble swaps to an inline editor pre-filled with its text. Only one editor open at a time; opening another cancels the first. Nothing else on screen changes (replies below stay visible).
2. Save & send with the text trimmed: empty or unchanged → treated as Cancel. Otherwise run the **normal send flow**, with the conversation considered to end just before the edited message: `effectiveMessages()` and the compaction slice use only the path up to the edit point, so the meter check and auto-compact consider exactly what the model will see.
3. When the send begins (`startSend`), commit: add the sibling node, make it active, rebuild the transcript below the edit point, show the user bubble and the streaming reply. If the send never begins (compaction stopped, error before streaming), the editor reopens with the text so nothing typed is lost, and the tree is unchanged.

### Retry flow

1. Retry click on the latest assistant reply → run the normal send flow with the path ending at its user message and **no** new user node.
2. When the stream starts, show the streaming bubble in place of the old reply (the old reply is not yet removed from the tree).
3. On finish or Stop with non-empty text: add the sibling assistant node and make it active; a compaction made for the retry is stored on that new reply (not on the user message), so the old reply keeps its own summary. On failure with no text: re-show the previous reply; the tree and every stored summary are unchanged.

### Version switcher

`‹` / `›` change the active child of the node's parent and rebuild below. The counter is `index+1 / siblings`. A new version is always appended at the end, so a freshly edited or retried message shows `n / n`.

### Interaction with the rest of the panel

- Clear chat empties the tree (as it empties history and compaction today).
- After any commit or switch, call `refreshContextUsage()`.
- The status line keeps its current messages ("Compacting…", "Generation stopped.", etc.). No new status copy is required except for a failed retry ("Retry failed; kept the previous reply.").
- Accessibility: pencil, retry and arrows get accessible names ("Edit message", "Retry reply", "Previous version", "Next version") and are reachable by keyboard.

## Testing

Pure logic first (no widgets), in a small testable header in the style of `ContextMeterLogic` from feature 1, tested from `--ui-check`:
- Tree operations: add message, edit (sibling creation), retry, switch, active-path derivation, sibling counts, edit inside an older version (branches again), switching a deep parent restores that branch's whole tail.
- Compaction state resolution: last snapshot on the active path; edit/switch before it drops it; switching back restores it; no snapshot → none.
- Notes (dividers) reappear on switching back.
Panel checks against the existing fixture backend (like feature 1's):
- Edit sends the right messages (path up to the edit point + new text); retry sends the path up to the user message; request bodies for usage/compact use the same prefix.
- Cancelling the editor, a stopped compaction before an edit, and a retry that returns nothing leave the tree and screen unchanged.
- Pencils/retry/arrows disabled during streaming and preparing; re-enabled after.
- Switcher shows the right counts and ends disabled; clear chat resets.
- `--theme-check` reports 0 failures; no UI failures beyond the 4 known.
Real model (SmolLM2-135M, as in feature 1): edit the first message of a 3-turn chat, confirm a different continuation, flip back and see the original chat intact; retry the last reply and flip between the two replies; edit after a compaction to confirm the earlier summary still applies.

## Left to the builder

File layout (new files for the tree logic and the switcher widget are fine); exact sizes, spacing and hover timing; whether bubbles are rebuilt or reused when the path changes; exact icon drawings (Lucide `pencil`, `chevron-left`, `chevron-right`); accessible names beyond those listed.

## Calls I made (to check while reading)

1. The reply below an edited message stays visible while you are editing, and is replaced when you press Save & send, so Cancel changes nothing.
2. Retry is offered on the latest reply only. Older replies change by editing the message above.
3. Compaction state is stored on the tree, so switching versions restores the right summary instead of forgetting it.
4. Editing never touches the backend or the model until you press Save & send.
5. Empty or unchanged edits are quietly treated as Cancel.
6. Pencil, retry and arrows are disabled while a reply is streaming or a send is being prepared.
7. Switcher is always visible when a message has more than one version (as in the chosen mockup option A); pencil and retry appear on hover.
8. Versions are in memory only until feature 3.
9. Re-running a message that got no reply is done through its editor (Save & send with unchanged text), not through a retry button, because the reply to retry does not exist. (Added after the final review.)
