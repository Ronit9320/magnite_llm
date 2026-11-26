# Tool mode fixes: design

Status: approved 2026-11-26, then revised after a review against the code. Branch: `pi-harness-fixes`.

## Intent

Tool mode should either do what was asked or show plainly that it did not. Today a request to create a
file fails without any sign on screen, and the model then keeps answering without acting. Longer,
multi-step tasks are the next goal. These fixes come first because such tasks would hit the same problems.

## What testing showed

Small tests, one run each, temperature 0.

- No tool can create a file. Without one, both Qwen3 0.6B and 1.7B fail the request.
- Actions rejected before approval (missing file, bad arguments) never appear on screen.
- A call to an unknown tool is shown to the user as raw `<tool_call>` text.
- `echo text > file` is accepted, exits 0 and creates nothing.
- On 0.6B, one answer without a call makes later turns skip tools. With `write_file` present, a short
  note on the newest user message took 11/24 to 21/24 (torch). On 1.7B it did not change the scores.
- Three-step tasks: a note after each tool result raised completed steps from 11/15 to 13/15 on 1.7B,
  and from 5/15 to 9/15 on 0.6B (llama.cpp).
- llama.cpp loads with a 2048-token context. A 6 KB file read fails the run.
- Models report results for steps they never ran.

## Goals

1. Create or replace a text file, after approval.
2. Every attempted action is visible: what it was, and whether it ran.
3. No raw tool markup as an answer.
4. Models use tools more reliably.
5. llama.cpp can hold a tool task.

Non-goals: replacing or restructuring Pi, editing part of a file, showing a diff, a working-folder picker,
keeping tool results between messages, raising the 6-turn and 4-action limits, torch memory growth.

Anti-goals: anything that writes, runs or opens without an explicit approval; an approval card that does
not show exactly what will happen.

## 1. `write_file`

Model tool: `write_file(path, content)`. New broker action `write`. The system prompt gains "To create or
change a file, call write_file.", and the plain JSON protocol (for models without a native tool format)
lists the new tool. The test results above included that prompt line.

Rules:

- The path is resolved before approval (`~` expanded, relative to the working folder, symlinks
  followed). The card shows the resolved path.
- The parent folder must already exist. An existing target must be a regular file.
- Content is UTF-8, up to 16,000 bytes, written exactly (no newline translation). Newlines and tabs are
  allowed. Other control characters and Unicode direction overrides are rejected, because they can make
  the preview read differently from the file.
- One model reply is capped at 768 tokens, so in practice only short files (roughly 2–3 KB) are possible.
  A longer call is cut off and the run reports that, as it does today.
- Writing inside Magnite's data folder is refused, except under `tool-workspace/`. Compare folders by
  file identity, not by spelling: macOS and Windows paths are usually case-insensitive.
- The approval fingerprint covers path, content and whether the file existed.
- At dispatch, a new file is opened exclusively, so one that appeared meanwhile fails the action. A
  replaced file must be unchanged since approval. It is written to a temporary file and renamed over the
  original, keeping its permissions.
- The Blueprint Tool approval node shares the broker, so it accepts `write` too.

Approval card:

```text
Replace file                                        Waiting
Path     /Users/you/test.md
This replaces the current contents (1.2 KB).
  from magnite space
Expires in 87s                          [Deny] [Approve once]
```

A new file reads "Create file" and has no warning line. The full content is plain text in a read-only
box that scrolls both ways, so long lines cannot be hidden. The app must label the new kind: today any
kind it does not know shows as "Open target".

## 2. Show what happened

- A rejected action gets a card with the status "Not run", the requested action and a plain reason: file
  does not exist, folder does not exist, not a regular file, shell syntax not supported, protected
  location, invalid request, or action limit reached. The model receives the same reason, with a hint
  where useful (for a missing file: "To create it, call write_file").
- Rejected attempts are held in the backend's memory only and returned with the run's approvals. They
  cannot be approved. They write no event, so a card may appear only when the answer arrives. After a
  restart they are gone.
- Approved and denied cards need no change: the app already keeps their details until it closes.
- A model's `run_command` line with unquoted `|`, `>`, `<`, `&`, `&&`, `||` or `;` is rejected as shell
  syntax. A leading `~/` in an argument is expanded and shown expanded.
- An answer from a run with no completed action gets a quiet caption beside the model name: "No actions
  ran". This includes ordinary replies such as an answer to "hi". Earlier turns of a reopened task are
  plain bubbles today and get no caption.

## 3. Unknown or half-written tool calls

The markup is removed. The answer reads "Nothing ran: the model asked for a tool Magnite does not have
(name)." A half-written call reads "Nothing ran: the model's tool call was incomplete." Each also records
a "Not run" card.

## 4. Reminders

Added only to what the model sees, never shown or saved:

- on the newest user message: "(Call a tool for this if it needs my computer.)"
- after a **completed** tool result, while actions remain: "(If my request has more steps, call the next
  tool now.)" It is not added after a denied, expired, rejected or failed action, so that it never pushes
  the model to try again.

If the model repeats a reminder in its answer (seen once on 0.6B), remove it.

Checked before planning (llama.cpp, 20 one-step requests, one run each): with the second reminder limited
to completed results, neither 0.6B nor 1.7B made any extra call, including on files whose text invites
one. Three-step results with that limit: 1.7B 3/5 completed (13/15 steps), 0.6B 1/5 (8/15). Once, on a
three-step task, 0.6B used the reminder turn to try opening a file instead of reading it; approval still
applies. Torch was not tested.

## 5. llama.cpp context

Load with 8192 tokens, or the model's own limit if smaller (today 2048). This also gives ordinary chat a
larger window and uses somewhat more memory. Tool output that would not fit is trimmed and marked as
truncated instead of failing the run.

## 6. Model hint

The Tool mode welcome text adds: "Very small models often miss steps. Qwen3 1.7B or larger is
recommended." Only the 1.7B Q8 GGUF on llama.cpp was tested; larger models and torch were not.

## Approaches considered

**Overwriting.** (a) Allow it behind a distinct "Replace file" card (chosen). (b) Create only: safer, but
"update my notes" cannot work.

**Where writes may go.** (a) Anywhere the user approves, except Magnite's data folder (chosen). Matches
reading and commands, and the original request was `~/`. (b) Home folder only. (c) Working folder only:
tightest, but needs a folder picker that does not exist.

**Action details.** The audit on disk stores no action details, and the storage guide says so. (a) Keep
that: details live in memory only (chosen). (b) Save them to `blueprints.db`: survives restart, but stores
paths and commands and changes the documented promise.

**Replacing Pi.** Not needed: it ran every step the model asked for. `pi-coding-agent` has file tools but
no per-action approval.

## Builder's calls

- No `DATA_SCHEMA` bump. The only new stored value is the audit kind `write`. An older build could not
  read those records after a downgrade. Raise this if the update policy needs a bump.
- Order: 1, then 2 and 3, then 4, then 5 and 6.
- Update `docs/BLUEPRINTS.md`, `docs/LOCAL_API.md` and `docs/STORAGE_AND_PRIVACY.md`.

## Done when

- The original conversation shows a "Create file" card, and the file exists after approval.
- A missing-file read shows a "Not run" card with its reason.
- No answer contains tool markup or a reminder.
- A 6 KB file can be read on llama.cpp.
- One-step requests make no more tool calls than they do without the reminders.
- Backend tests, `--theme-check` and the Tool mode selftest (extended with write approve and deny) pass.
- The tests above are re-run on 0.6B and 1.7B against the built code.
