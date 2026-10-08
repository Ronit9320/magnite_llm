# Troubleshooting

| Symptom | Check / next step |
| --- | --- |
| Backend unavailable | Launch from a terminal; keep the binary at `build/magnite-app`; check `.venv/bin/python` and backend installation. Run `PYTHONPATH=backend/src .venv/bin/python -m backend.main --port 0` for startup details. |
| Qt not found by CMake | Install Qt 6 base and SVG development packages, not only runtime libraries. |
| llama-cpp-python installation fails | Read the native build error; confirm CMake/compiler availability and the chosen package's hardware build requirements. |
| Model cannot load | Match GGUF to llama.cpp and transformer weights to torch/vLLM. Check the saved global engine override and available RAM/VRAM. |
| Training dependencies missing | Use the same `.venv` as the desktop and install `-e 'backend[ml]'`. |
| Quantization/CUDA failure | Confirm torch and bitsandbytes support the installed GPU/runtime. Try unquantized LoRA only if the model fits; a smaller model may be needed. |
| Out of memory | Unload inference before training; use smaller weights, batch size, sequence/context length, or supported quantization. GPU sharing is not yet release-verified. |
| Download denied or offline | Check repository spelling, network access, model license/access approval, and HF authentication. Confirm required weights are cached before offline use. |
| Cached HF models missing | Restart after updating, then refresh the local library. Verify `HF_HUB_CACHE` or `HF_HOME/hub` in the desktop's environment. Adapter-only, tokenizer-only and incomplete snapshots are not base models. See [discovery rules](development/BACKEND_PROCESS.md#4-model-library-discovery-pulls-and-deletion). |
| RAG unavailable | Install ML extras; allow the first embedding-model download or prepopulate the cache. PDF parsing also needs pypdf. |
| No usable PDF text | Scanned/image-only PDF OCR is not implemented; supply extracted text. |
| No retrieved matches | Confirm documents are indexed; test the query in Knowledge; review similarity threshold and embedding model. |
| Context exceeds limit | Start a shorter conversation, reduce requested output tokens, or reduce retrieved context. |
| Adapter import rejected | Use a complete Magnite export ZIP; duplicate IDs and invalid archives are rejected. |
| Adapter chat fails | Ensure the matching transformer base model and PEFT/torch dependencies are accessible. GGUF-only base weights do not satisfy this path. |
| Cancel appears delayed | Native loading, embedding, or generation may need to return before cooperative cancellation completes. |
| Data directory change has no effect | Restart the app; check MAGNITE_HOME precedence. Existing data is not moved automatically. |
| vLLM startup fails | Check optional installation/hardware and `vllm.log` under application data. |

For bug reports include the commit/version, OS, Python/Qt versions, hardware,
engine/model format, minimal reproduction, and a sanitized error excerpt. Do not
attach private datasets, model credentials, or your whole application store.
Use [security reporting](../SECURITY.md) for vulnerabilities.
