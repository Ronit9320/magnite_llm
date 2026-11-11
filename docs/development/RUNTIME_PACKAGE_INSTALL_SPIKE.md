# Runtime package install spike (Part 3, Task 3.0)

Question: can the packaged runtime install a missing package at the user's request without touching the signed bundle?

Checked 2026-11-11 on macOS 27.0.1 arm64 against the installed `Magnite.app` (read-only; installs went to a scratch directory).

## Findings

| Check | Result |
| --- | --- |
| `pip` in the packaged runtime | Present (`pip 26.0.1`, Python 3.12). No `uv`. |
| `pip install --target <user dir>` with the bundled python | Works for pure-Python and native (`.so`) wheels. |
| Load under `python -I` | **Not** visible by default (`-I` ignores `PYTHONPATH` and user site). Works after `sys.path.append(<user dir>)` in backend code; native extension loaded from there. |
| Bundle signature | `codesign --verify --deep --strict` stays valid because nothing is written inside the app. |
| Signing flags | Ad-hoc (`flags=0x2`), no hardened runtime, so unsigned libraries from outside load today. A Developer-ID/notarised build would enforce library validation and needs `com.apple.security.cs.disable-library-validation` (or user-installed native packages will fail to load). |
| Already bundled (macOS arm64) | `llama-cpp-python 0.3.35`, `torch 2.14.0`, `peft 0.20.0`. |
| Not bundled | `vllm` — no macOS wheel on PyPI (Linux `manylinux` only; macOS is an sdist). The app also never selects vLLM on macOS (`supervisor.py:75`). |

## Consequences

- Install target must be a user-writable directory (e.g. `<app_home>/packages`), appended to `sys.path` at backend start so bundled copies win.
- On a packaged macOS build every installable catalogue entry is already present and vLLM is unsupported, so the Install button would never appear there. It matters for source installs without `[ml]`, and for Linux (vLLM).

## Hazard found while running the spike

Running the bundled interpreter without `-B` writes `__pycache__` files into the app and invalidates its seal ("file added"). Always pass `-B` (the app itself does). Any new code path that launches the runtime python must keep `-B`.
