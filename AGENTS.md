# AGENTS.md

Native LLM desktop app: C++20/Qt6 client in `app/`, Python FastAPI backend in `backend/`. The client spawns and supervises the backend on a random `127.0.0.1` port; the C++ shell never does ML.

## Build & run

```bash
pip install -e "backend[ml]"        # omit [ml] for GGUF inference without training/RAG (torch/peft/bitsandbytes lazy)
cmake -S app -B build
cmake --build build -j
./build/magnite-app
```

- Agents: do not run an unbounded `cmake --build build -j` — it can hang the machine. Build in two limited steps instead: `cmake --build build --target magnite-app_autogen -j 4`, then `cmake --build build --target magnite-app -j 4`.
- New C++ files must be added to `MAGNITE_SOURCES` in `app/CMakeLists.txt` (explicit list, AUTOMOC/AUTORCC on). Qt6 Widgets + Network + Svg and OpenSSL Crypto; `-Wall -Wextra`.
- Source builds look up the repo from the binary's location: use `<repo>/build/magnite-app` for `backend/src` and `.venv` to resolve. Packaged builds instead use the isolated Python under `runtime/python` beside the executable. See `packaging/README.md` for the test AppImage recipe and checks.
- Backend data lives in `~/.magnite` on Linux/macOS and `%APPDATA%/Magnite` on Windows (override `MAGNITE_HOME`). Run it standalone for debugging: `magnite-backend --port 0 --home ~/.magnite`. Desktop launches require a per-launch Bearer credential except for `/health`; tokenless standalone launches omit credential checks. The CLI enforces loopback Host values and a data-directory lock on POSIX/Windows; direct Uvicorn startup bypasses that wrapper.
- `shared/` is an empty placeholder; the IPC contract lives in `backend/src/backend/routes/` and the C++ `BackendClient`.

## Verify

CI in `.github/workflows/verify.yml` covers the base backend, CMake, theme/UI and desktop selftest; `native-packages.yml` builds and tests the Linux/macOS/Windows packages through reusable `build-{linux,macos,windows}.yml`. Both run automatically for relevant code/packaging/workflow changes on pushes to `dev`, `master` and `release/**` and on PRs targeting them; ordinary documentation-only changes skip them. Feature branches do not run on push. Keep workflow files tracked and use branch filters. Nothing publishes automatically: `release.yml` is the only publisher, run by hand on a `release/**` branch (see `docs/development/RELEASE_PIPELINE.md`). `linux-preview.yml` is a manual Linux-only build that never publishes. See `docs/development/NATIVE_PACKAGES.md` for current evidence and limitations. Offline backend regressions: `PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests -v`. UI and real-model verification use the binary's built-in tests:

```bash
./build/magnite-app --theme-check                                  # token/QSS/font/icon audit; prints "0 failure(s)"
MAGNITE_SELFTEST_PULL=1       ./build/magnite-app --selftest        # model pull e2e
MAGNITE_SELFTEST_INFERENCE=1  ./build/magnite-app --selftest        # load -> chat (llama.cpp) -> unload
MAGNITE_SELFTEST_TRAINING=1   ./build/magnite-app --selftest        # LoRA job e2e
MAGNITE_SELFTEST_ADAPTER=1   ./build/magnite-app --selftest        # train + hot-switch e2e
MAGNITE_SELFTEST_RAG=1       ./build/magnite-app --selftest        # ingest -> query -> delete e2e
MAGNITE_SELFTEST_TOOLMODE=1  ./build/magnite-app --selftest        # Tool mode tab: load -> send -> approve/deny e2e
./build/magnite-app --screenshot <dir>                             # capture panels, then exit
```

Selftests download tiny toy models (e.g. `sshleifer/tiny-gpt2`) on first use; backend readiness takes ~13s. Pass prints `[selftest] PASS (0 failures)`.

The Tool mode selftest needs Node 22.19+ and the Pi bridge packages (`npm ci --ignore-scripts --prefix backend/src/backend/tooling/pi`), and loads `Qwen/Qwen3-0.6B` with the torch engine (override with `MAGNITE_SELFTEST_TOOLMODE_MODEL` / `_ENGINE`).

## Branch workflow

- Start ordinary changes on a new branch from `dev`. Merge completed work into `dev` for testing, then promote tested `dev` changes to `master`.
- Releases are cut from `dev` onto `release/<name>` branches (the current one is `release/magnite-spacev0.2`, v0.2-preview). Fixes reach a release branch by cherry-pick from `dev`; never merge a release branch back into `dev` or `master`.
- Do not develop directly on `dev` or `master`. Delete completed branches only after verifying their commits are merged into `dev`; preserve unmerged work.

## Preview identity and updates

- `backend/src/backend/_version.py` is the canonical release identity; CMake, backend metadata and packaging derive from it. Python distribution metadata uses a computed PEP 440 spelling.
- To ship another preview, increase both `RELEASE_VERSION` and `UPDATE_SEQUENCE` (and set `RELEASE_NAME`, the GitHub release title) on the release branch. Published versions are never overwritten; `release.yml` refuses a reused version or a sequence that is not higher than every published one.
- `backend/src/backend/_features.py` is the build profile. `BLUEPRINTS_AND_TOOLS = False` (release branches) removes the Blueprints workspace, Tool/Blueprint conversation modes, their backend routes and the bundled Node.js/Pi; the code still builds and its tests still run on `dev`.
- In-app updates read `update-<platform>-<arch>.json` (+ `.sig`) for Linux, macOS and Windows. The macOS update package is `Magnite-<version>-macos-arm64.zip`; Windows reuses the setup `.exe`. The post-exit install steps are `packaging/update_helper.cpp` (Linux), `packaging/macos/update_install.sh` and `packaging/windows/update_install.ps1`, bundled as Qt resources.
- `DATA_SCHEMA` is an update compatibility contract, not a migration framework. Bump it when persistent formats change; the updater rejects schema changes.
- Never commit `.release-secrets/` or replace the update public key without planning trust/key rotation. See `docs/development/RELEASE_PIPELINE.md` for architecture, signing and verification.

## Current theme

- Design tokens are inline functions in `app/src/theme.h`; `@token` placeholders in `app/resources/theme/console.qss` (embedded as `:/theme/theme.qss`) are substituted at runtime by `Theme::qss()`. **Never hardcode hex values in widget code or QSS** — add a token to `theme.h` and extend `Theme::selfCheck()`.
- Icons are Lucide-style SVGs requiring `stroke="currentColor"`; tint via `Theme::coloredIcon()`.
- The finalized design is implemented in `app/src/theme.h`, `app/resources/theme/console.qss`, and `app/resources/brand/`. Follow existing components and the theme rules in `CONTRIBUTING.md`. Retired design experiments are not specifications.

## Backend quirks

- `torch`/`transformers`/`peft`/`bitsandbytes` are imported lazily inside engines so the server starts without the `[ml]` extras.
- transformers v5: no `warmup_ratio` in `TrainingArguments` (use `warmup_steps`); the progress bridge must subclass `TrainerCallback`; target-module lists fall back to `all-linear` when names don't exist on the architecture.
- Adapter hot-switch is only reliably testable through the in-process PEFT/torch engine; llama.cpp only loads GGUF adapters at load time, vLLM needs `--enable-lora` at subprocess launch.

## Docs

Architecture, implementation plans, process maps, and analysis reports live in `docs/development/`. Within that directory, `app.md` and `BUILD_PLAN.md` remain gitignored (local-only) — they won't exist in fresh clones. Public instructions live in `README.md`, `CONTRIBUTING.md`, `docs/`, and `RELEASE_CHECKLIST.md`. The release scope excludes hosted inference and cloud training.
