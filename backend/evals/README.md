# RAG embedder evaluation

Developer tool, not shipped in the app. Compares embedding models on your own documents on this machine:
recall@1/4/10, MRR, ingest speed, load time and peak RAM.

1. Put your documents (`.txt`, `.md`, `.html`, `.pdf`) in one directory.
2. Write `questions.jsonl`, one line per question:
   `{"question": "...", "doc": "file name", "answer_contains": ["phrase that must appear in the right passage"]}`
3. Run (needs the `[ml]` extras; downloads each model on first use):

```bash
.venv/bin/python backend/evals/rag_eval.py --all --docs <dir> --questions questions.jsonl --out results.md
```

Each candidate runs in its own process. A memory guard (`backend/src/backend/rag/memguard.py`) checks free memory before loading, caps the Apple-GPU allocator, polls the real footprint (including GPU memory) and stops the run (exit 75) before the machine swaps; limits via `MAGNITE_RAG_MAX_MB`, `MAGNITE_RAG_MIN_FREE_MB`, `MAGNITE_RAG_MAX_SWAP_MB`. Chunk size is set per model from its
sequence limit (about 512 tokens, never more than the model can embed); override with `--chunk-words`.
The metric functions are covered by `backend/tests/test_rag_eval.py`.
