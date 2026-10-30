# v0.1 macOS test .dmg — 2026-10-30

This is a local test artifact, not a public or stable release. Work is on `release/v0.1-macos`. No tag, public release, notarization or
remote publication is part of this build. The evidence for every claim below is in [`LOG.md`](LOG.md) (dated entries) and
[`ML_MATRIX.md`](ML_MATRIX.md) (the only source of ML claims).

## Artifact and environment

- `dist/Magnite-0.1.0-test.1-arm64.dmg`, 335,068,085 bytes (about 320 MiB).
- SHA-256 `480508361da193be7b8bfb9cf39caff3829073baa9b14ad93aeadabf76ce61de`, recorded in `dist/SHA256SUMS-macos`.
  `(cd dist && shasum -a 256 -c SHA256SUMS-macos)` printed `OK` on 2026-10-30.
- Apple silicon (arm64) only. Minimum macOS 14.0 (Sonoma), enforced by `LSMinimumSystemVersion` and by `verify_bundle.py`, which fails any
  Mach-O whose `minos` exceeds 14.0 ([ADR 0002](adr/0002-minimum-macos.md)).
- Built and tested on one Mac: Apple M1, 8 GB, macOS 27.0.1 (build 26A434), Apple clang 14.0.3 (Command Line Tools 14.3.1, SDK 13.3,
  no full Xcode), CMake 4.4.3, Qt 6.8.3. Full host record: [`environment-macos.txt`](environment-macos.txt).
- Bundled python-build-standalone Python 3.12.12, torch 2.14.0 (CPU/MPS), transformers 5.16.1, peft 0.20.0, sentence-transformers 6.0.1,
  huggingface_hub 1.33.0, and llama-cpp-python 0.3.35 built from source with Metal (`GGML_METAL=ON`, embedded shaders). bitsandbytes is
  not included. Pins: `packaging/macos/requirements-macos-arm64.txt`; resolved set: `packaging/macos/constraints-macos-arm64.txt`.
  The resolved list for the shipped runtime is also in `runtime-packages-macos.txt` and, inside the app, `Contents/Resources/build-info/`.
- Model and embedding weights, caches, application data, vLLM and Ollama are not included.
- Qt and third-party licence texts ship in the bundle (`packaging/macos/licenses/`, produced by `qt_notices.py`).
  **The maintainer's legal sign-off on these notices (ISSUE-009) is still pending.**

## Provenance

- `build.json` inside the image records commit `ef173fd` with `working_tree_modified: true`. The fixes for ISSUE-001, -005, -007, -008,
  -009, -010 and -011 were uncommitted when the dmg was built (2026-10-28 02:41) and were committed afterwards as `8337e65`.
- By file modification time, every source file in `8337e65` under `app/`, `backend/` and the runtime/packaging inputs is older than the
  dmg. Only the tester-kit scripts (`clean_install_collect.sh`, `stage_clean_install.sh`) and documentation changed after it. This is an
  inference from timestamps, not a rebuild. A byte-for-byte rebuild from `8337e65` has **not** been done, so the artifact is not
  claimed to be reproducible from a commit.

## What this build is

The client launches the bundled interpreter (`Contents/Resources/runtime/python/bin/python3 -B -I`) and never falls back to a host
Python once a runtime directory exists. The backend binds to loopback on a random port and requires a per-launch secret and a loopback
`Host` header; a second copy on the same data folder refuses to start ([ADR 0009](adr/0009-local-api-authentication.md)). The bundle is
ad-hoc signed.

## Verification

All results below were produced on the machine above; see `LOG.md` for the commands and output.

- Offline backend regressions: **65 tests OK** via `bash packaging/macos/run_regressions.sh` (re-run 2026-10-30, exit 0). The plain
  desktop CMake build was re-checked after each shared-file edit.
- `check_dmg.sh`: 0 failures (mount read-only → layout → bundle smoke test → detach). Bundle gate (`verify_bundle.py`): 0 failures.
- `verify_installed.sh` on the app copied out of the final dmg: **11/11 PASS**, with a poisoned `PYTHONHOME`/`PYTHONPATH`, a scrubbed
  `PATH` and a disposable `MAGNITE_HOME`: `--theme-check`, `--ui-check`, `--selftest`, and the PULL, INFERENCE, TRAINING, ADAPTER and RAG
  selftests, `live_workflow.py` (registered model → chat → training → adapter → RAG → export/delete/import → restart), `--screenshot`,
  and an unchanged signature afterwards. These ran with `QT_QPA_PLATFORM=offscreen`.
- Lifecycle on `/Applications` (Phase 9, run on an earlier build, commit `5a04358`): a normal quit leaves no backend; replacing the app
  in place and removing it both preserve `~/.magnite` and preferences. The same run showed a force-killed app orphaning its backend
  (ISSUE-001). The macOS parent watchdog that fixes this was verified later on the packaged app: the backend exited 1.0 s after
  `kill -9` of the GUI (2/2). Phase 9 was not re-run as a whole on the final dmg.
- Clean-install proxy (Phase 10): a quarantined copy of the dmg was installed from a second, standard account (`tester`) into
  `/Applications` and opened through the Gatekeeper override. `smoke_bundle.sh` passed 6/6 with the backend running under the bundled
  `python3 -B -I`, and one real chat with a 135M GGUF model replied in about a second (reported by the maintainer, not timed).
- ML paths, from [`ML_MATRIX.md`](ML_MATRIX.md) (one M1, models up to 1.5B parameters, 12-step training runs):
  - GGUF chat via llama.cpp on Metal: supported.
  - Transformer inference on MPS (float16): supported.
  - LoRA training on MPS (float32): supported; about 23× faster than CPU.
  - Adapter hot-switch serving and RAG embeddings on MPS: supported.
  - QLoRA 4-bit and 8-bit: **not supported on macOS**. The option is disabled in Training and `/training/start` returns 400 with an
    explanation.

## Known limitations

- **First GPU load pause.** The first GPU model load for a macOS user pauses about 10 s while Metal shaders compile (ISSUE-002). Phase 10
  did not time model load in the second account, so the cold cost for a new user is unverified.
- **Gatekeeper.** The build is not notarized (ISSUE-003, accepted for v0.1); see "Not claimed".
- The local API requires a per-launch secret held in the app's environment. It is hidden from other accounts but visible to processes of
  the same user (ADR 0009).
- Full list, including fixed issues: [`ISSUES.md`](ISSUES.md).

## Not claimed

- **No Developer ID signing, no notarization, no stapling.** The signature is ad-hoc. Gatekeeper blocks the first launch of a
  downloaded copy; the user must use Privacy & Security → Open Anyway. Phase 11 is blocked on an Apple Developer Program enrollment,
  a Developer ID Application certificate and a `notarytool` profile ([ADR 0005](adr/0005-signing-approach.md)).
- **The Gatekeeper dialog wording was not captured**, so it is not quoted anywhere. The flow itself (blocked → Privacy & Security →
  Open Anyway → launched) was completed in Phase 10.
- **No truly clean machine.** The proxy account still sees machine-wide Homebrew (`/opt/homebrew`) and `/usr/local/bin/python3`. The app
  ignored both, but that is not the same as a Mac without them.
- **The cross-account API refusal (`CLEAN_INSTALL.md` §7b, ISSUE-011) was not run.** It was shown only from the developer account with
  `curl` (401 without or with a wrong secret, 400 for a fake Host, `/health` open on purpose).
- No Intel Mac, no Rosetta, no macOS 14 or 15 (only 27.0.1 was used), no Mac with less than 8 GB or with a Pro/Max GPU.
- No interactive (non-offscreen) GUI session was scripted; interactive use was by the maintainer only.
- No models larger than 1.5B parameters, no training longer than 12 steps, no prompt-prefill benchmarks.
- No vLLM, Ollama, CUDA or bitsandbytes.
- No Linux or Windows evidence; Windows has no data-folder lock (`fcntl`).
- No public-redistribution certification.

See the [tester instructions](../../../packaging/macos/TESTING.md) for how to run the build without touching existing data.
