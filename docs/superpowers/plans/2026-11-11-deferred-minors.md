# Deferred Minors Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans (the owner chose native execution) to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the one deferred review finding that can affect user data (a healthy database moved aside for a non-corruption error), and record why the other four are not worked.

**Architecture:** `open_or_quarantine` currently quarantines any `sqlite3.DatabaseError` that is not an `OperationalError`. Replace that with a positive test for real corruption: the SQLite error code (`SQLITE_CORRUPT` 11, `SQLITE_NOTADB` 26, extended codes included) when the exception carries one (Python 3.11+), else the two known corruption messages (Python 3.10, which the app still accepts). Everything else propagates and the file is left alone.

**Tech Stack:** Python 3.10+ (dev 3.13, packaged 3.12), sqlite3, unittest.

**Spec:** the whole-branch review in `.superpowers/sdd/2026-11-10-break-report-fixes/progress.md` ("Final: minor (deferred)" lines) and `docs/testing/BREAK_REPORT_2026-11-10.md`.

## Global Constraints

- Branch `fix`. **Never push, open a PR, or dispatch a workflow without asking.** Do not commit unless the owner asks (they asked once, for the earlier work).
- Same loop as before: break it first, test red, fix, test green, re-run the same break, full suite green.
- Offline suite: `PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests` (309 tests today, must stay green).
- A real corruption must still be quarantined exactly as before (garbage file, truncated file, malformed image), including its `-wal`/`-shm`/`-journal` files, with a notice.

## Review Focus

1. **A real corrupt file must still be caught on every Python version the app ships**: 3.10 has no `sqlite_errorcode`; the message fallback must cover both real corruption messages. Pinned by the message-only test.
2. **Extended error codes**: SQLite reports e.g. `SQLITE_CORRUPT_VTAB` (267); the primary code is the low byte. Pinned by a test using code 267.
3. **A code beats a message**: an exception with a non-corruption code (e.g. 19, constraint) must not be quarantined even if its text mentions "malformed". Pinned by a test.

---

### Task 1: Quarantine only real corruption (deferred minor #2)

**Files:**
- Modify: `backend/src/backend/database.py` (`open_or_quarantine`, ~line 56)
- Test: `backend/tests/test_break_fixes_2026_11.py` (class `Finding1TolerantStartup`)
- Modify: `docs/testing/break_recheck.py` (new in-process probe `1b`)

**Interfaces:**
- Produces: `database._is_corruption(exc: sqlite3.DatabaseError) -> bool`. True when `getattr(exc, "sqlite_errorcode", None)` is an int whose low byte is 11 or 26; when no code is available, true when the message contains `"file is not a database"` or `"database disk image is malformed"` (case-insensitive); otherwise false. `open_or_quarantine` quarantines only when `_is_corruption(exc)`; any other exception propagates unchanged and the database file is untouched.

- [ ] **Step 1: Break it first.** Add probe `1b` to the harness: on a healthy database, run `open_or_quarantine` with an `init` that raises `sqlite3.IntegrityError("UNIQUE constraint failed")`; HELD when the database is still in place with no `.corrupt-*` file, BROKEN when it was moved aside. Run `break_recheck.py --only 1b`. Expected: `BROKEN` today.
- [ ] **Step 2: Write the failing tests** (in `Finding1TolerantStartup`, each with a healthy `models.db` created first):
  - `test_integrity_error_is_not_corruption` and `test_programming_error_is_not_corruption`: `init` raises the error; assert it propagates, no `.corrupt-*`, the database still exists.
  - `test_message_only_corruption_is_quarantined`: `init` raises `sqlite3.DatabaseError("database disk image is malformed")` once, then succeeds; assert one `.corrupt-*` file and one notice (this is the Python 3.10 path).
  - `test_corruption_error_code_is_quarantined`: an exception with `sqlite_errorcode = 11`, then `= 26`, then `= 267` (extended `SQLITE_CORRUPT_VTAB`), each with an unrelated message; assert each is quarantined.
  - `test_error_code_beats_message`: `DatabaseError("database disk image is malformed")` with `sqlite_errorcode = 19`; assert it propagates and nothing is moved.
- [ ] **Step 3: Run** `-k Finding1`. Expected: the integrity, programming and code-beats-message tests FAIL (today everything that is a `DatabaseError` is quarantined); the message-only test passes today (it is the guard).
- [ ] **Step 4: Implement `_is_corruption`** and use it in `open_or_quarantine`'s `except sqlite3.DatabaseError`; keep the `OperationalError` re-raise. Update the function's docstring.
- [ ] **Step 5: Run** `-k Finding1`, then the full suite. Expected: PASS, 309 plus the new tests.
- [ ] **Step 6: Re-break.** `break_recheck.py --only 1,1b`. Expected: `#1 HELD` (a real garbage `models.db` and a bad job row still recover) and `#1b HELD`.

---

### Task 2: Record the four deliberately skipped minors

**Files:** `.superpowers/sdd/2026-11-10-break-report-fixes/progress.md` only.

- [ ] **Step 1:** Append one ledger ruling per skipped minor with its reason: chunked 400 vs 413 (memory still bounded, the app's client always sends `Content-Length`); stop-event leak (only after a thread-limit failure); cancelled-swap reload (a few seconds of "busy", nothing lost); `--ui-check`/`--screenshot` auto-restart (**the review was partly wrong**: `--ui-check` never starts a backend, and disabling restart for `--screenshot` would turn a masked crash into a blocking dialog on a dev tool).
