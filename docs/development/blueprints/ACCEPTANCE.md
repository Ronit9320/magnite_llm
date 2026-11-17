# Blueprint acceptance

The November 18, 2026 Linux acceptance used an isolated data profile and locally cached
checkpoints. The public workflow, capability matrix, and permission limits are
in the [Blueprints guide](../../BLUEPRINTS.md).

## Reproducible desktop verification

Build Magnite and install the [source Pi dependencies](PI_INTEGRATION.md#development-runtime).
Cache complete model snapshots for
`HuggingFaceTB/SmolLM2-135M-Instruct` and `Qwen/Qwen3-0.6B` in the app's
Hugging Face cache. The first checkpoint is used for real adapter training,
and the second exercises thinking support and Pi.

```sh
QT_QPA_PLATFORM=offscreen HF_HUB_OFFLINE=1 \
MAGNITE_BLUEPRINTS_LIVE_CHECK=1 \
MAGNITE_BLUEPRINTS_ACCEPTANCE_MODEL=HuggingFaceTB/SmolLM2-135M-Instruct \
MAGNITE_BLUEPRINTS_LIVE_MODEL=Qwen/Qwen3-0.6B \
./build/magnite-app --ui-check design/blueprints-preview/native-acceptance
```

On Linux CPU, the check opened the order starter from the library and created
a separate draft. It bound SmolLM2, queued real LoRA training in the node dialog,
and bound the completed job as revision 2. The trained node passed its
isolated test. The editor and chat reopened and ran revision 2 with identical
results. A Reasoning test rejected SmolLM2 with the explicit thinking
requirement and produced the typed result `393` with Qwen3.

The same clean-profile check approved `python3 --version`, denied a URL
request from the linked inspector after reconnecting, and ran a separate
Qwen3 Tool mode task through Pi. All checks passed. The order starter's
accuracy is measured separately in the [held-out order benchmark](ORDER_BENCHMARK.md).

The offline backend suite passed 437 checks with one macOS-only skip. The
native build, the 16 packaging tests and the update manifest CTest passed.
Local logs and screenshots are under `build/packaging/blueprints-phase6/`
and `design/blueprints-preview/native-acceptance/`.
For an offline GGUF smoke check, set `MAGNITE_SELFTEST_INFERENCE_MODEL` to an
absolute path to a complete cached GGUF file. The default selftest fixture
uses the Hub to look up the model file by repository name.

## Package verification

Native builders package the Node 22.23.5 executable selected by
`packaging/node.lock.json`. They restore Pi 1.5.2 and its production
dependencies with npm scripts disabled. Each package records the dependency
versions and runs a Pi smoke check using the installed worker and an offline
provider stub. The fixture test and the real Qwen check have separate results.
[Official Node archive hashes](https://nodejs.org/en/blog/release/v22.23.5/)

The Linux development host is Arch x86-64. Both local AppImages report a
minimum glibc of 2.43. The release CI still gates its Ubuntu 22.04 package at
2.35. Local package artifacts and logs are under
`build/packaging/blueprints-phase6/`. The default native package jobs
produce their usual artifacts with ML support. The additional base Linux
AppImage is a separate verification build.
