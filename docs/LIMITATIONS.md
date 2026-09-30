# Preview limitations

- Source installation depends on the checkout layout. There are no verified distributable packages yet.
- Linux development tests do not establish clean-install or macOS/Windows support.
- Dependencies are not locked; GPU/quantization combinations need explicit verification.
- vLLM and Ollama integrations have mock regression coverage but need live release checks.
- The local API has custom SSE frames and no application authentication. Public/LAN serving is outside scope.
- Training is LoRA/QLoRA adapter training, not cloud/full-model training.
- Adapter activation restarts the torch engine; native multi-LoRA and GGUF adapter conversion are not implemented.
- Cancellation is cooperative. Download/ingestion progress and independent job management need further work.
- RAG chunk sizes are words; some backend schema labels still say tokens. PDF OCR and automatic embedding-model reindexing are absent.
- Large-corpus retrieval, large-model OOM recovery, forced termination, and concurrent training/inference need broader verification.
- Chat history is not durable. Backup/restore across changed paths or future schema versions needs manual care.
- Tiny-model test passes establish workflow delivery, not useful answer quality or fine-tuning effectiveness.

See the [release checklist](../RELEASE_CHECKLIST.md) for the work required before publishing.
