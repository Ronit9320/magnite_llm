# Release preparation verification — 2026-09-29

Base application commit: `e5bc39903d9738581043a209c3bd20d2767f3477`. Documentation, example inputs, license,
repository templates, and CI configuration were added in the working tree. No
application source, CMake configuration, or Python dependency declaration changed.
This is preparation evidence, not certification of a future release candidate.

## Environment

- Arch Linux, x86_64 development machine.
- Python 3.12.12 in the existing repository venv.
- Qt 6.11.2, CMake 4.4.3, GCC 16.2.1 (20260810).
- Qt offscreen platform for desktop checks; disposable data/settings directories.
- Feature tests used the existing model cache and ML environment. A separate fresh base-only environment was also installed and tested as described below.
- [Direct dependency inventory](../../THIRD_PARTY_NOTICES.md) records installed versions.

## Results

| Check | Result |
| --- | --- |
| CMake configure and build, `-j2` | PASS in the working checkout and a clean exported source tree |
| Fresh base backend installation | PASS in a new Python 3.12.12 venv, including native llama-cpp-python compilation |
| Fresh-environment regressions/theme/desktop | PASS: 24 tests (0.265 seconds), 0 theme failures, and desktop/IPC PASS |
| Existing offline backend suite | PASS, 24 tests in 5.875 seconds |
| Theme audit | PASS, 0 failures |
| Bare desktop selftest | PASS, including backend readiness and four error recovery scenarios |
| Model pull selftest | PASS, tiny-gpt2 registered; network metadata enabled |
| GGUF inference selftest | PASS, 8 streamed deltas and unload; network metadata enabled |
| Training selftest | PASS, 3 steps and adapter registration; cached/offline weights |
| Adapter selftest | PASS, activation, 6 deltas, and clear; cached/offline weights |
| RAG selftest | PASS, ingest/query/delete/cleanup; cached/offline weights |
| Combined live workflow | PASS, registry load/chat, mixed dataset training, adapter + RAG chat, archive roundtrip, and durable history |
| Example dataset ingestion | PASS: JSONL 4 rows, CSV 4 rows, plain text 1 normalized row |
| Example Markdown extraction/chunking | PASS, opening-hours fact preserved |
| Screenshot captures | PASS, six panels in dark and light themes; empty/connecting preview states |
| CI workflow | YAML structure checked locally; first hosted execution pending |
| Publication pattern scan | No matches in the checked credential/path patterns; limited scope documented separately |

The sandbox blocked localhost binding for the first desktop test and stalled the
first offline async test request. The successful reruns used permitted execution
outside that restriction. Initial offline pull/inference attempts failed because
those paths query Hugging Face metadata even when weights are cached; they passed
when rerun with network access. These initial attempts are not counted as passes.

## Fresh source/base installation

Exported the unchanged application commit with `git archive HEAD` into a temporary
source tree, created a new `.venv`, installed `-e backend` from the package index,
and configured/built its own `build/magnite-app`. No system-site packages were
enabled. Re-ran all 24 offline regressions, the theme audit, and the bare desktop
selftest using that tree and venv; all passed.

The fresh resolution includes huggingface_hub 2.0.0, uvicorn 0.54.0, and NumPy
2.5.3, while the existing ML venv has older versions of those dependencies. The
[base environment snapshot](base-environment.txt) records all installed packages
excluding the editable project. It is evidence for this base-only check, not a
cross-platform lockfile or verification of the ML extras on these newer versions.

The initial package-index attempt was blocked by sandbox network restrictions;
the installation succeeded with permitted network access. The C++ build used
already installed system Qt/compiler libraries, so this is a clean source/venv
check on the development host, not a clean OS/VM test.

## Reproduce

From the repository root with the backend installed:

```bash
cmake -S app -B build
cmake --build build -j2
PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests -v
QT_QPA_PLATFORM=offscreen ./build/magnite-app --theme-check
```

Use separate disposable MAGNITE_HOME and XDG_CONFIG_HOME directories for each
feature check, then run the commands in [CONTRIBUTING.md](../../CONTRIBUTING.md).
The three training/adapter/RAG checks and combined workflow were run with
HF_HUB_OFFLINE=1 against the existing cache. Pull/GGUF selftests need network
metadata access with the current implementation.

Local raw logs are under `/tmp/magnite-release-review/`; they are temporary and
not part of the public repository. This report preserves the commands, environment,
and outcomes without exposing machine-specific model paths or runtime logs.

## Limits

No new source fixes were made. The fresh base source/venv check does not establish
a clean-machine OS installation or a fresh ML-extras installation, live vLLM/Ollama support, macOS/Windows support, large-model quality,
OOM behavior, large-corpus scaling, IPC security, all forced-exit paths, or binary
redistribution readiness. Screenshot inspection covers static layout, not a full
accessibility audit or successful interactive workflow recording. Tiny-model
text delivery is not an answer-quality evaluation.

The GitHub Actions workflow targets Ubuntu 24.04/Python 3.12 with base dependencies;
that hosted environment has not been executed here. It installs dependencies from
package indexes and intentionally does not run real-model/GPU tests. Final
candidate verification must be repeated after the planned source changes.
