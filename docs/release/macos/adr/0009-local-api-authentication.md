# ADR 0009 — Authenticate the local backend API (per-launch secret + Host check + data-folder lock)

- Date: 2026-10-28
- Status: **Accepted and implemented** (2026-10-28, uncommitted) — see ISSUE-011 for the evidence

## Context

The app starts the backend on a random `127.0.0.1` port and talks to it over HTTP with no credentials. This was proven on 2026-10-28: any
local process — including other users' — can make the backend read arbitrary files, and a foreign `Host` header is accepted (DNS
rebinding). The maintainer also asked whether to run the backend as a separate service.

## Options considered

| Option | Security | Cost / risk | Verdict |
|---|---|---|---|
| A. Leave as is | proven exposure | none | rejected |
| B. Separate always-on backend (launchd agent) | **worse**: always listening, longer exposure window | install/uninstall leftovers, version skew, port discovery, RAM held when the app is closed | rejected |
| C. Unix-domain socket with owner-only permissions | strongest: the OS blocks other users; no port, no DNS rebinding | rewrite of all app networking — `QNetworkAccessManager` cannot speak HTTP over a Unix socket, so requests, SSE streaming and downloads would need a custom client; Windows (future) differs | deferred — revisit if C's strength is ever needed |
| D. Secret token stored in a file | good | a long-lived secret on disk; only helps external tools | rejected |
| **E. Per-launch secret in the environment + Host allow-list + data-folder lock** | blocks other users, other processes and DNS rebinding | small: ~4 places in the client, 1 middleware, 1 lock | **chosen** |

## Decision (E)

1. **Per-launch secret.** On every start, `BackendManager` generates a random 256-bit token (`QRandomGenerator::system()`). It passes the
   token to the backend in the environment (`MAGNITE_API_TOKEN`), **never on the command line**: any user can read other users' command
   lines with `ps`, but not their environment (measured 2026-10-28). The backend removes the variable on startup so child processes do not
   inherit it. The same user can still see the initial environment through `ps -E`, which is outside the threat model.
2. **The backend requires it** on every request (`Authorization: Bearer …`, constant-time compare). The response is 401 otherwise.
   `/health` stays open: it only says "ok", and the app uses it to know when the backend is ready.
3. **Host allow-list, always on:** only `127.0.0.1`, `localhost` and `[::1]` (with or without the port). Anything else gets 400. This blocks
   DNS rebinding even in developer mode.
4. **Developer mode:** a backend started by hand without `MAGNITE_API_TOKEN` skips the token check (Host check still on) and logs a warning,
   so `magnite-backend --port 0` and the tests keep working.
   The middleware wraps the app in `backend/main.py` (the network-facing server), not inside `server.py`, so in-process tests and
   `live_workflow.py` (which use a `testserver` host) are unaffected.
5. **One backend per data folder:** the backend takes an OS lock (`fcntl.flock`) on `MAGNITE_HOME/.backend.lock`. The OS releases it when
   the process dies, even on `kill -9`, so a lock can never go stale.
   - If another backend holds the lock, it retries for a few seconds (covering the ~1 s exit of a just-crashed predecessor, ISSUE-001).
   - Then it exits with a dedicated code, and the app says "Magnite is already running" instead of "Backend crashed".
6. **Client:** `BackendClient` adds the header to its three request types (GET, POST, download, including streaming chat). The token is kept
   in memory only.

## Consequences

- Other users, other processes and web pages get 401/400. The app works exactly as before.
- Two Magnite instances can no longer write the same `~/.magnite` databases at once.
- Linux gets the same protection (shared code). It is not testable on this Mac.
- Not covered: malware running as the same user. It could read the app's memory or `~/.magnite` directly anyway, which is outside a local
  API's threat model.
