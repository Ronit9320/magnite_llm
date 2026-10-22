# ADR 0004 — ML dependency strategy on Apple Silicon

- Date: 2026-10-23
- Status: Accepted (verification pending in Phase 6)

## Context

`packaging/requirements-cpu.txt` pins the Linux test build. On macOS arm64, three
of those pins behave differently (all checked against PyPI on 2026-10-23):

- `torch==2.14.0` — `macosx_14_0_arm64` wheel exists. The Linux build forces
  `--torch-backend cpu`; on macOS there is no such variant, and the same wheel
  provides MPS. `backend/src/backend/inference/engines/torch.py:43` already
  selects `mps` when available, and `detect_gpu()` already reports
  "Apple Silicon (Metal/MPS)".
- `llama-cpp-python==0.3.35` — **sdist only**, so it must be compiled. The Linux
  recipe disables AVX/CUDA; macOS needs the Metal flags instead. The host has no
  Metal shader compiler (`xcrun metal` → "unable to find utility", because only
  Command Line Tools are installed), so a default Metal build would fail.
- `bitsandbytes==0.50.2` — a `macosx_14_0_arm64` wheel does exist, which is newer
  news than the Linux build's assumptions. Whether 4-bit/8-bit actually works on
  Apple Silicon is unknown; `trainer.py` passes `BitsAndBytesConfig` straight
  through, and `trainer.py:292` gates `fp16` on CUDA only.

## Options

- llama.cpp: Metal on with `GGML_METAL_EMBED_LIBRARY=ON` (shaders embedded as
  source, compiled at runtime by the Metal driver — no build-time toolchain), or
  Metal off (CPU only), or install full Xcode purely to get the shader compiler.
- Quantization: assume it works; assume it does not; or measure it.

## Decision

1. Fork the pins into `packaging/macos/requirements-macos-arm64.txt`, keeping the
   same versions so the two platforms stay comparable.
2. Build `llama-cpp-python` from sdist with
   `-DGGML_METAL=ON -DGGML_METAL_EMBED_LIBRARY=ON -DGGML_NATIVE=OFF -DLLAMA_CURL=OFF`.
   If that fails twice, fall back to `-DGGML_METAL=OFF` and ship CPU-only
   llama.cpp, recorded as a limitation. Do not install Xcode for v0.1.
3. Keep `bitsandbytes` in a **separate optional requirements file** so a failed
   quantization probe cannot break the runtime build, and treat quantized
   (QLoRA) training as **unsupported until Phase 6 proves otherwise** — the same
   stance the Linux build took.
4. Phase 6 measures every cell (llama.cpp Metal/CPU, torch CPU/MPS inference,
   LoRA training on CPU and MPS, 4-bit, 8-bit) into
   `docs/release/macos/ML_MATRIX.md`. Only measured cells may be advertised.

## Consequences

- The advertised macOS ML matrix is whatever Phase 6 measures, nothing more.
- `GGML_NATIVE=OFF` means the shipped llama.cpp is not tuned to this particular
  M1, which is correct for a redistributable package.
- A llama.cpp Metal failure degrades performance but not correctness; a torch MPS
  failure narrows training to CPU; neither blocks producing the `.dmg`.
