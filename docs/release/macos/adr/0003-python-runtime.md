# ADR 0003 — Python runtime: python-build-standalone inside Contents/Resources

- Date: 2026-10-23
- Status: Accepted

## Context

The Linux AppImage already solved this: `packaging/prepare_runtime.py` copies an
Astral python-build-standalone 3.12.12 installation next to the client, installs
the backend as a normal wheel (never editable, never the developer venv), and the
client launches it with `-I -B` so a user's `PYTHONPATH`/`PYTHONHOME` cannot
substitute modules. An incomplete packaged runtime never falls back to a system
interpreter. macOS should reuse that contract rather than invent one.

Two macOS-specific wrinkles:

- Bundle layout. `QCoreApplication::applicationDirPath()` is
  `Magnite.app/Contents/MacOS`. Putting a ~10k-file Python tree there mixes
  resources into the executable directory; the Apple-conventional place for
  non-executable payload is `Contents/Resources`.
- Code signatures. python-build-standalone binaries arrive ad-hoc signed. Any
  `install_name_tool` edit invalidates that signature and makes the binary fail
  to launch on arm64 until re-signed.

## Options

1. `Contents/MacOS/runtime` — byte-identical to the Linux layout, zero C++ change.
2. **`Contents/Resources/runtime`** — conventional, keeps `Contents/MacOS` to the
   single client binary; needs one guarded probe path in `BackendManager`.
3. Embed Python (link libpython) — a rewrite of the process model; rejected.

## Decision

`Contents/Resources/runtime/python`, probed first on macOS with the existing
`applicationDirPath()/runtime` retained as a fallback so the Linux layout keeps
working verbatim. Copy the runtime unmodified — no `install_name_tool`, no
relocation of its dylibs — and ad-hoc re-sign inner Mach-O files after staging.

## Consequences

- One `#ifdef Q_OS_MAC` block in `app/src/backend_manager.cpp` (Phase 4).
- The `-I -B` isolation and "never fall back to system Python" guarantees are
  unchanged across platforms.
- Because nothing inside the runtime is rewritten, breakage from invalidated
  signatures is avoided by construction rather than repaired afterwards.
