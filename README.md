# Magnite LLM

A native desktop app for the full open-source LLM workflow — download a model, fine-tune it (LoRA/QLoRA), attach RAG, switch LoRA adapters, and chat with it locally. Hosted inference is planned, not implemented. No browser, no Electron, no telemetry.

- **Client:** C++20 / Qt 6 Widgets application
- **Backend:** Python FastAPI service (PyTorch, PEFT, bitsandbytes, llama.cpp, vLLM)
- **Communication:** local HTTP/SSE on `127.0.0.1`, random port; the client spawns and supervises the backend
- **Theme:** Studio 03 — graphite/orange dark and cream/orange light, centralized tokens; see [the committed design notes](design/studio-3/README.md).

---

## Features

### Implemented

- **Model registry & downloads** — pull GGUF / HF models via HuggingFace Hub, track local metadata (architecture, size, license) in `~/.magnite`
- **Local fine-tuning** — LoRA / QLoRA jobs via PEFT (+ bitsandbytes), log/metric streaming, cancel and status polling
- **Adapter store** — train, import/export, list, delete, and activate LoRA adapters using a restarted PEFT/torch engine
- **Local inference** — llama.cpp (CPU/GPU), vLLM, or torch engines; streaming chat & completion over SSE with a blinking-cursor chat UI; engine auto-selected by hardware with manual override
- **Studio UI** — global theme tokens, native controls, accessible navigation, chat bubbles, light/dark modes and reduced motion

- **RAG** — PDF/TXT/Markdown/HTML extraction, sentence-transformer embeddings, persisted SQLite/NumPy cosine retrieval, configuration and chat-context injection. Requires `[ml]`; chunk sizes are word counts.
- **Audit hardening** — validated adapter imports, registry-model resolution, cancellable worker-based token streaming, visible IPC errors, serialized training with durable job history, and offline regression tests.

### Roadmap

- **Phase 6 hardening:** ingestion jobs/progress, retrieval controls and large-corpus scaling; the existing RAG panel is functional.
- **Phase 7 — hosted inference:** OpenAI-compatible endpoints. Hosted controls are disabled until this exists.
- **Phase 8 — cloud full fine-tuning:** mock dispatcher first, real provider later.
- **Phases 9–10:** remaining persistence/UX, broader tests/CI and distributable packages.

---

## Architecture

```
┌────────────────────────────── Magnite Desktop (C++/Qt6) ──────────────────────────────┐
│                                                                                        │
│   Models │ Training │ RAG │ Adapters │ Chat │ Settings      ← stacked panels            │
│        ▲        ▲        ▲       ▲         ▲                                            │
│        └────────┴────────┴───────┴─────────┴─── BackendClient (HTTP/SSE)                │
│                                                 ▲                                       │
│                                      BackendManager (spawn/supervise)                   │
└──────────────────────────────────────────────────┼──────────────────────────────────────┘
                                                   │ 127.0.0.1:<random port>
                                      ┌────────────┴──────────────┐
                                      │  Magnite Backend (Python) │
                                      │  FastAPI + uvicorn        │
                                      │  ├ /models   registry     │
                                      │  ├ /training LoRA/QLoRA   │
                                      │  ├ /adapters store        │
                                      │  ├ /inference engines     │
                                      │  └ /health                │
                                      └────────────┬──────────────┘
                                                   │
                              HF Hub (downloads) · local model/vector caches · adapters
```

The C++ shell never performs ML work — it orchestrates, displays, and persists state.

---

## Repo layout

```
app/                        Qt 6 desktop client (CMake)
  src/theme.h|.cpp          design tokens + QSS compiler (single source of truth)
  src/panels/               Models, Training, RAG, Adapters, Chat, Settings panels
  src/widgets/              FlowLayout (card grids)
  resources/theme/theme.qss   global stylesheet (@token placeholders)
backend/                    Python ML backend (FastAPI)
  src/backend/routes/       REST routers (models, training, adapters, inference, RAG, health)
  src/backend/inference/    engine supervisor (llama.cpp / vLLM / torch)
  src/backend/training/     LoRA/QLoRA job manager + trainer
  src/backend/models/       model registry
design_doc.md               space-theme UI spec (+ flagged amendments)
BUILD_PLAN.md               phase plan + phase 5.5 delivery notes
PHASE_5_5.md                UI redesign phase spec (5.5.0–5.5.7)
app.md                      product & architecture overview
```

---

## Requirements

| Component | Requirement |
| --- | --- |
| Compiler | C++20 (GCC / Clang, `-Wall -Wextra`) |
| Qt | Qt 6 (Widgets, Network, Svg) — `find_package(Qt6 ...)` in `app/CMakeLists.txt` |
| CMake | ≥ 3.20 |
| Python | ≥ 3.10 |
| ML deps | optional extras — `torch`, `transformers`, `peft`, `bitsandbytes`, `sentence-transformers`, `datasets`, `accelerate`, `safetensors` |

System requirements table (VRAM per model tier) is in `app.md`.

---

## Getting started

```bash
# 1. Clone
git clone <this repo>
cd magnite_llm

# 2. Python backend (inside a venv)
python -m venv .venv && source .venv/bin/activate
pip install -e "backend[ml]"          # omit [ml] for CPU-only inference

# 3. Build the desktop app
cmake -S app -B build
cmake --build build -j

# 4. Run (the app starts its own backend on a random localhost port)
./build/magnite-app
```

> The backend installs a `magnite-backend` console script. You don't normally run it by
> hand — the app spawns it — but you can for debugging:
> `magnite-backend --port 0 --home ~/.magnite`

Data lives in `~/.magnite` (`MAGNITE_HOME` overrides it).

---

## Developer / testing

### CLI flags

| Flag | What it does |
| --- | --- |
| `--theme-check` | Verify theme tokens, embedded QSS/fonts/icons; prints `0 failure(s)` or fails |
| `--screenshot <dir>` | Capture each panel to `dir/panel-<n>.png`, then exit (works on Wayland; in-process grab) |
| `--selftest` | Run the UI/integration self-test suite, then exit |

### Selftests

```bash
./build/magnite-app --selftest                          # UI + settings round-trips
MAGNITE_SELFTEST_PULL=1       ./build/magnite-app --selftest   # model pull e2e
MAGNITE_SELFTEST_INFERENCE=1  ./build/magnite-app --selftest   # load → chat (llama.cpp) → unload
MAGNITE_SELFTEST_TRAINING=1   ./build/magnite-app --selftest   # LoRA job e2e
MAGNITE_SELFTEST_ADAPTER=1    ./build/magnite-app --selftest   # train + hot-switch adapter e2e
MAGNITE_SELFTEST_RAG=1        ./build/magnite-app --selftest   # ingest -> query -> delete e2e
```

Each prints `[selftest] PASS (0 failures)`. The backend is spawned automatically; the
`INFERENCE`/`TRAINING`/`ADAPTER` runs download a small model/adapter toy set on first use.

### Offline backend regressions

```bash
PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests -v
```

Tests use temporary stores and simulated external services; ML downloads are not required. The optional real workflow test is `PYTHONPATH=backend/src .venv/bin/python backend/tests/live_workflow.py`; it uses temporary data and may download tiny model weights. Built-in feature selftests still exercise real tiny models. The bare selftest now requires backend readiness and tests SSE errors, HTTP errors, network disconnects and truncated streams.

### Runtime settings

Global engine selection applies to the next model load when Chat selects Auto. The data-directory setting applies on the next app start; `MAGNITE_HOME` takes precedence and existing data is not moved. HF downloads use the Hugging Face cache rather than the registry directory. Hosted API fields are disabled, and unused legacy plaintext API keys are removed from QSettings.

Local inference accepts registered model IDs, compatible local paths and raw HF GGUF repository IDs. Auto chooses llama.cpp for local GGUFs, torch/vLLM for registered transformer weights, and Ollama for Ollama records. Adapters currently use a PEFT/torch restart; clearing an adapter unloads the engine. Native vLLM multi-LoRA and llama.cpp adapter conversion remain future work.

The local IPC stream format is `data: {"delta": "..."}`, followed by `{"done": true}` or an explicit `{"error": "..."}`. `stream:false` returns an OpenAI-shaped JSON completion. This is not a drop-in OpenAI server.

### Theme workflow

All colors/fonts/radii are tokens (`@bgVoid`, `@accentPrimary`, …) defined in
`app/src/theme.h` and substituted into `app/resources/theme/theme.qss` at runtime by
`Theme::qss()`. **Do not hardcode hex values** in widget code or the QSS — add a token and
extend `Theme::selfCheck()`. (Enforced by the Phase 5.5.7 centralization audit.)

---

## Verification status

See [the dated audit and appended follow-up](AUDIT_2026-09-27.md) for exact checks, defect resolution, and unverified hardware/services. Tiny-model passes do not establish large-model quality, memory capacity, or cross-platform support.

The active Studio design is authorized by `design_doc.md` §13; the earlier blue Space specification is historical. Committed design notes live in `design/studio-3/README.md`.

---

## Privacy

Local-first by default. No telemetry, no data collection. Training, inference and document processing run locally. Downloading model or embedding weights contacts the configured model hub; Ollama requests go to the configured Ollama service. Hosted inference and cloud fine-tuning are not implemented.