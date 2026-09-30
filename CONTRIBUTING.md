# Contributing to Magnite

Work within the local desktop scope: model management, LoRA/QLoRA training,
local inference, adapters, and RAG. Hosted/cloud features are outside this release.
Start with [installation](docs/INSTALL.md), [the release checklist](RELEASE_CHECKLIST.md),
and [AGENTS.md](AGENTS.md).

## Project structure

- `app/`: C++20/Qt 6 Widgets client. BackendClient handles HTTP/SSE; BackendManager supervises Python.
- `backend/src/backend/`: FastAPI routes and local model, training, inference, adapter, and RAG services.
- `backend/tests/`: offline regressions and the opt-in live workflow.
- `app/resources/` and `design/studio-3/`: current theme resources and committed design rules.

Keep ML work in Python and heavyweight imports lazy. New C++ files must be added
to MAGNITE_SOURCES in app/CMakeLists.txt. Preserve explicit error reporting,
cooperative cancellation, and lifecycle serialization. Route/schema changes must
update the client and [API documentation](docs/LOCAL_API.md) together.

## Theme rules

Use the current Studio 03 design; earlier palettes are historical. Add colors to
`app/src/theme.h`, extend Theme::selfCheck, and reference tokens in QSS. Do not
hardcode hex colors in widgets/stylesheets. Icons use `stroke="currentColor"`
and Theme::coloredIcon. Preserve keyboard focus, both themes, and reduced motion.
The [committed design notes](design/studio-3/README.md) are available in fresh clones;
ignored local planning files are not required contribution references.

## Verification

From the repository root:

```bash
cmake -S app -B build
cmake --build build -j2
PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests -v
QT_QPA_PLATFORM=offscreen ./build/magnite-app --theme-check
```

Use disposable data/settings for desktop checks:

```bash
export MAGNITE_HOME="$(mktemp -d /tmp/magnite-test-data.XXXXXX)"
export XDG_CONFIG_HOME="$(mktemp -d /tmp/magnite-test-config.XXXXXX)"
QT_QPA_PLATFORM=offscreen ./build/magnite-app --selftest
```

Feature checks use real tiny models and may download weights. Run separately:

```bash
MAGNITE_SELFTEST_PULL=1 QT_QPA_PLATFORM=offscreen ./build/magnite-app --selftest
MAGNITE_SELFTEST_INFERENCE=1 QT_QPA_PLATFORM=offscreen ./build/magnite-app --selftest
MAGNITE_SELFTEST_TRAINING=1 QT_QPA_PLATFORM=offscreen ./build/magnite-app --selftest
MAGNITE_SELFTEST_ADAPTER=1 QT_QPA_PLATFORM=offscreen ./build/magnite-app --selftest
MAGNITE_SELFTEST_RAG=1 QT_QPA_PLATFORM=offscreen ./build/magnite-app --selftest
PYTHONPATH=backend/src .venv/bin/python backend/tests/live_workflow.py
```

The live workflow creates its own temporary store. MAGNITE_HOME does not isolate
shared model caches. Do not use personal weights for destructive deletion tests.
The bare desktop selftest must fail if the backend cannot start; socket-blocked
sandbox failures are not application success. Record environment limitations.

## Pull requests

Explain the user-visible problem, resulting behavior, and relevant verification.
Add regression tests for meaningful behavior changes, especially errors, storage,
and lifecycle behavior. Keep unrelated formatting/refactors separate. Include
screenshots for visible UI changes and describe unverified hardware/service paths.
Do not add credentials, private datasets, downloaded weights, or generated build trees.

Original contributions are submitted under the project's [MIT license](LICENSE).
Identify third-party sources and preserve their terms. Report sensitive defects
through [SECURITY.md](SECURITY.md), not a public reproduction containing secrets.
