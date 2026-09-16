# Magnite LLM

A native desktop app for the full open-source LLM workflow — download a model, fine-tune it (LoRA/QLoRA), attach RAG, switch LoRA adapters, and chat with it locally or via a hosted endpoint. No browser, no Electron, no telemetry.

- **Client:** C++20 / Qt 6 Widgets application
- **Backend:** Python FastAPI service (PyTorch, PEFT, bitsandbytes, llama.cpp, vLLM)
- **Communication:** local HTTP/SSE on `127.0.0.1`, random port; the client spawns and supervises the backend
- **Theme:** centralized space theme — dark, flat-black, glassmorphic panels, blue accent (see `design_doc.md`)

---

## Features

### Implemented (Phases 1–5.5)

- **Model registry & downloads** — pull GGUF / HF models via HuggingFace Hub, track local metadata (architecture, size, license) in `~/.magnite`
- **Local fine-tuning** — LoRA / QLoRA jobs via PEFT (+ bitsandbytes), log/metric streaming, cancel and status polling
- **Adapter store** — train, import/export, list, delete, and **hot-switch** LoRA adapters against the loaded base model
- **Local inference** — llama.cpp (CPU/GPU), vLLM, or torch engines; streaming chat & completion over SSE with a blinking-cursor chat UI; engine auto-selected by hardware with manual override
- **Space UI pass (Phase 5.5)** — global theme tokens + single QSS sheet, glass cards, chat bubbles, icons (Lucide-style, HiDPI-aware), 150ms panel crossfade, full verification suite

### Roadmap

- **Phase 6 — RAG pipeline:** document ingestion (PDF/TXT/MD/HTML), chunking, embeddings, local vector store, retrieval-context injection at inference time (panel is currently a placeholder)
- **Phase 7 — cloud full fine-tuning:** dispatch training jobs to resold compute (provider research pending)
- **Phase 8 — hosted inference routing** (OpenAI-compatible endpoints)

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
  src/resources/theme.qss   global stylesheet (@token placeholders)
backend/                    Python ML backend (FastAPI)
  src/backend/routes/       REST routers (models, training, adapters, inference, health)
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
```

Each prints `[selftest] PASS (0 failures)`. The backend is spawned automatically; the
`INFERENCE`/`TRAINING`/`ADAPTER` runs download a small model/adapter toy set on first use.

### Theme workflow

All colors/fonts/radii are tokens (`@bgVoid`, `@accentPrimary`, …) defined in
`app/src/theme.h` and substituted into `app/resources/theme/theme.qss` at runtime by
`Theme::qss()`. **Do not hardcode hex values** in widget code or the QSS — add a token and
extend `Theme::selfCheck()`. (Enforced by the Phase 5.5.7 centralization audit.)

---

## Verification status

- Phase 5.5 (space-theme UI redesign) is **done** — delivery notes in `BUILD_PLAN.md`, deviations recorded in `design_doc.md` §10.
- Phase 6 (RAG) is the next milestone; the RAG panel is a placeholder mock.
- Bare + PULL / INFERENCE / TRAINING / ADAPTER selftests all green; `--theme-check` green; idle CPU ≈ 0.5% (no animation loops).

---

## Privacy

Local-first by default. No telemetry, no data collection. Local-only workflows (model download, LoRA/QLoRA, adapter switching, local inference) never leave the device. Cloud full fine-tuning (Phase 7) is opt-in per job and its data-retention policy is still under review.