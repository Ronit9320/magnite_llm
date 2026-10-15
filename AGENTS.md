# AGENTS.md

Native LLM desktop app: C++20/Qt6 client in `app/`, Python FastAPI backend in `backend/`. The client spawns and supervises the backend on a random `127.0.0.1` port; the C++ shell never does ML.

## Build & run

```bash
pip install -e "backend[ml]"        # omit [ml] for CPU-only inference (torch/peft/bitsandbytes lazy)
cmake -S app -B build
cmake --build build -j
./build/magnite-app
```

- New C++ files must be added to `MAGNITE_SOURCES` in `app/CMakeLists.txt` (explicit list, AUTOMOC/AUTORCC on). Qt6 Widgets + Network + Svg; `-Wall -Wextra`.
- Source builds look up the repo from the binary's location: use `<repo>/build/magnite-app` for `backend/src` and `.venv` to resolve. Packaged builds instead use the isolated Python under `runtime/python` beside the executable. See `packaging/README.md` for the test AppImage recipe and checks.
- Backend data lives in `~/.magnite` (override `MAGNITE_HOME`). Run it standalone for debugging: `magnite-backend --port 0 --home ~/.magnite`.
- `shared/` is an empty placeholder; the IPC contract lives in `backend/src/backend/routes/` and the C++ `BackendClient`.

## Verify

CI is configured in `.github/workflows/verify.yml` for the base backend, CMake build, theme audit, and bare desktop selftest; its first hosted run still needs verification. Offline backend regressions: `PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests -v`. UI and real-model verification use the binary's built-in tests:

```bash
./build/magnite-app --theme-check                                  # token/QSS/font/icon audit; prints "0 failure(s)"
MAGNITE_SELFTEST_PULL=1       ./build/magnite-app --selftest        # model pull e2e
MAGNITE_SELFTEST_INFERENCE=1  ./build/magnite-app --selftest        # load -> chat (llama.cpp) -> unload
MAGNITE_SELFTEST_TRAINING=1   ./build/magnite-app --selftest        # LoRA job e2e
MAGNITE_SELFTEST_ADAPTER=1   ./build/magnite-app --selftest        # train + hot-switch e2e
MAGNITE_SELFTEST_RAG=1       ./build/magnite-app --selftest        # ingest -> query -> delete e2e
./build/magnite-app --screenshot <dir>                             # capture panels, then exit
```

Selftests download tiny toy models (e.g. `sshleifer/tiny-gpt2`) on first use; backend readiness takes ~13s. Pass prints `[selftest] PASS (0 failures)`.

## Branch workflow

- Start every change on a new branch from `dev`.
- Merge completed work into `dev` for testing, then promote tested `dev` changes to `master`.
- Do not develop directly on `dev` or `master`. Delete completed branches only after verifying their commits are merged into `dev`; preserve unmerged work.

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
