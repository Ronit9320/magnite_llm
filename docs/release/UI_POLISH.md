# UI polish verification — 2026-09-30

Candidate: the commit introducing this report on `design/intuitive-motion`, based
on `fb04c00`. Find its exact identity with
`git log -1 --format=%H -- docs/release/UI_POLISH.md`.
This report covers the requested UI refinement, not completion of every public
release gate in [RELEASE_CHECKLIST.md](../../RELEASE_CHECKLIST.md).

## Result

All six workspaces share refined controls, typography, keyboard focus and compact
layouts. The original native vector identity now references the Konark wheel,
with detailed and small-size variants. Training, conversation and click feedback
follow real operation states and respect reduced motion.

The pass also repairs actionable UI behavior: duplicate submission and overlapping
loads, cancellation/error recovery, document selection and stale query results,
long content wrapping, atomic adapter export, destructive confirmations and
settings feedback. Hosted settings, the unused telemetry option and the ineffective
Keep adapter option are removed. Existing engine preference indices remain
Auto=0, vLLM=1 and llama.cpp=2; legacy hosted credential cleanup remains.
The incorrect FP8 label now reads “8-bit (bitsandbytes)”.

The design notes, wheel exports, and previews from this historical pass have
been retired. The current supplied brand assets supersede the wheel identity;
see [current theme rules](../../CONTRIBUTING.md) and [screenshot instructions](SCREENSHOTS.md).
Populated previews use synthetic UI fixtures. Empty full-app previews use disposable
stores and may show Connecting during initial backend startup.

## Environment and checks

Development host: Arch Linux, GCC 16.2.1, CMake 4.4.3, Qt 6.11.2,
Python 3.12.12. Desktop checks used Qt's offscreen platform with loopback access.
Real-model checks used disposable `MAGNITE_HOME` and `XDG_CONFIG_HOME` directories
and the existing model cache. No personal model registry was modified.

| Check | Result |
| --- | --- |
| `cmake --build build -j4` | Pass; C++ warnings enabled |
| `QT_QPA_PLATFORM=offscreen ./build/magnite-app --theme-check` | 0 failures; both themes and text-token contrast checks |
| `QT_QPA_PLATFORM=offscreen ./build/magnite-app --ui-check` | 0 failures; all six panels, both themes, compact/standard widths |
| Same UI check with `QT_SCALE_FACTOR=2` | 0 failures |
| `PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests -v` | 24 tests passed |
| Bare `./build/magnite-app --selftest` with offscreen Qt | PASS, 0 failures; backend startup and chat error recovery |
| `MAGNITE_SELFTEST_PULL=1` desktop selftest | PASS, 0 failures; tiny model downloaded/registered |
| `MAGNITE_SELFTEST_INFERENCE=1` desktop selftest | PASS, 0 failures; llama.cpp load, 8 streamed deltas, unload |
| `MAGNITE_SELFTEST_TRAINING=1` desktop selftest | PASS, 0 failures; 3 training steps and adapter registration |
| `MAGNITE_SELFTEST_ADAPTER=1` desktop selftest | PASS, 0 failures; train, torch adapter activation, 6 deltas, clear |
| `MAGNITE_SELFTEST_RAG=1` desktop selftest | PASS, 0 failures; ingest, query, delete |

Each feature flag was run separately with `QT_QPA_PLATFORM=offscreen` and
`./build/magnite-app --selftest`. Cached training/adapter/RAG runs used
`HF_HUB_OFFLINE=1`; pull/inference allowed model metadata requests. The final
8-bit label correction was followed by rebuilding and repeating the UI check.

The committed `--ui-check` uses an in-process loopback fixture server and isolated
QSettings. It covers keyboard card selection, literal filenames, cancelled deletion,
query success/error, download retry, training terminal states and duplicate start
protection, chat clear during streaming, long unbroken responses, reduced motion,
wheel animation, button activation/ripple cleanup and atomic export preservation.
Layouts are checked at 848/1104-pixel workspace widths (1024/1280-pixel app windows),
including expanded training controls. Optional screenshot output makes the results
visually reviewable. CI now invokes this check after the theme audit.

## Limits and remaining release gates

Token contrast checks enforce at least 4.5:1 for selected text/background pairs;
they are not a full accessibility certification. Native window-system interaction,
screen readers, macOS/Windows, live vLLM/Ollama, representative large-model quality,
OOM/resource contention and clean-machine installation were not verified in this
pass. The combined `backend/tests/live_workflow.py` was not rerun here; its earlier
results are historical. Tiny model delivery verifies plumbing, not answer quality.

Hosted CI still needs a successful run. Publication/security-contact, dependency
provenance, packaging and the remaining lifecycle/security gates remain tracked in
the release checklist. No merge, push, tag or public release was performed.
