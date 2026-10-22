# ADR 0008 — Merge the `mac` branch health-probe fix into the macOS release branch

- Date: 2026-10-23
- Status: Accepted

## Context

While auditing the environment it turned out that branch `mac`, commit
`7fcdb3c` (2026-10-04, "Probe backend health over a raw loopback socket"), is
**not merged into `dev`**:

```
$ git log --oneline dev..mac
7fcdb3c Probe backend health over a raw loopback socket
```

Its message records the symptom: on macOS the `QNetworkAccessManager` health
request issued by `BackendManager` never reached the backend, so the app stayed
"Unavailable" even though the backend was up. The fix probes `/health` with a
plain `QTcpSocket`, keeping the retry and `started` logic intact. The diff is
confined to `app/src/backend_manager.{h,cpp}` (+54 / −22).

Branching `release/v0.1-macos` from `dev` therefore starts from a tree where the
app cannot reach its own backend on macOS — the packaged build would fail every
functional acceptance check for a reason unrelated to packaging.

## Options

1. Branch from `mac` instead — loses the newer `dev` commits (`0fa53e3`,
   `2249fcb`, `94f4f10`) that the release candidate should contain.
2. **Branch from `dev` and merge `mac`** — keeps both, with the merge visible in
   history and revertible on its own.
3. Cherry-pick `7fcdb3c` — same content, but leaves `mac` looking unmerged and
   invites a duplicate commit when `mac` is later merged to `dev`.
4. Reimplement the probe — pointless; the fix exists and is already attributed.

## Decision

Branch `release/v0.1-macos` from `dev` (done in Phase 0) and
`git merge --no-ff mac` as the first action of Phase 1, before any packaging
work, so the macOS baseline is measured on a tree where the client can actually
reach the backend.

## Consequences

- `release/v0.1-macos` contains the macOS fix; promoting this branch to `dev`
  later also lands `mac`, after which `mac` can be deleted per the branch
  workflow in `AGENTS.md` (verify merged, then delete).
- Phase 1's acceptance check includes seeing `backend health OK` in the selftest
  output, which is the direct observable for this fix.
- If the merge conflicts (it should not — `dev` has not touched
  `backend_manager.cpp` since), stop and record it in `ISSUES.md` rather than
  resolving the client's health logic by guesswork.
