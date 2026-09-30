# Demo recording runbook

Target: a short local-workflow recording with synthetic inputs. No recording of a
successful real workflow is claimed yet; this runbook is ready for the candidate build.

1. Use a disposable MAGNITE_HOME and XDG_CONFIG_HOME. Pre-download a compatible,
   redistributable model and embeddings; record the model ID and hardware used.
2. Show Models and load a verified model. Ask a short prompt and show streaming.
3. Import `examples/training.jsonl`; show training configuration and a completed
   small LoRA run. If time is compressed, label the edit honestly.
4. Show the resulting adapter, activate it, and send a prompt. Do not claim the
   handful of sample rows measurably improves quality.
5. Ingest `examples/observatory.md`, retrieve the library opening hours, then ask
   the same question with RAG enabled. Show the source passage for comparison.
6. Export the adapter and show the saved archive; end with the source installation link.

Avoid private paths, credentials, personal model names, datasets, and documents.
Record usable model output, not random-model selftest text. Keep disabled hosted
controls out of promotional footage until the source-code pass removes them.

Current automated panel captures are indexed in [screenshots](SCREENSHOTS.md).
