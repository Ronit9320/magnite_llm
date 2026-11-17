# RAG Engine Update, Phase A Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Magnite's RAG handle large documents on an 8 GB machine: a long-context embedder behind a clean abstraction, chunks sized to that embedder, a cached search index, whole-passage context packing, reindexing to a new model, and batched, cancellable, bounded-memory ingest.

**Architecture:** SQLite (`rag/rag.db`) stays the only durable store, with the same tables and columns. A derived in-memory index (float16 or int8, capped at 512 MB) is rebuilt lazily and invalidated on every write, so nothing needs syncing. Embedding goes through an `Embedder` (preset specs, query/document prompts, half-precision load, idle unload, optional MRL truncation). Ingest and reindex share one batched embedding loop with cancel checks and progress.

**Tech Stack:** Python 3.10+ (venv is 3.13), NumPy, sqlite3, FastAPI/Pydantic, sentence-transformers (lazy import), `unittest`. No new dependencies.

**Spec:** [RAG_RESEARCH_REVIEW.md](RAG_RESEARCH_REVIEW.md) (findings, model comparison, section 7 task list); [architecture.md](architecture.md) "Retrieval and document processing" and "Extension points and constraints"; [../LIMITATIONS.md](../LIMITATIONS.md). This file replaces the earlier plan: its Tasks 1, 2, 3, 5 carry over (renumbered, extended); its Task 4 ("report the token limit") and Task 6 are superseded.

## Global Constraints

- Phase A only. Out of scope: hybrid FTS5 search (Phase B); reranker, GGUF embedding backend, Docling (Phase C).
- Branch `rag-engine-update` from `fix` (the most up-to-date line; it contains all of `origin/dev`). Never develop on `dev`/`master`. Untracked docs in the tree (`PI_HARNESS_ARCHITECTURE.md`, `RAG_*.md`) stay untracked or are carried, never lost. Never commit `brag-output-*`.
- No on-disk format change: same tables and columns in `rag.db`, so `DATA_SCHEMA` in `backend/src/backend/_version.py` is **not** bumped. An existing `rag.db` must open and search unchanged. New `rag_config` keys are allowed (it is key/value).
- IPC is additive only (new routes, new response keys, new optional config keys). Existing fields, including `tokens`, and the chat `sources` SSE event keep their names and shapes. Do not touch `app/`.
- `torch` and `sentence_transformers` are imported lazily. The base backend starts and the offline suite runs without the `[ml]` extras; tests use fake encoders.
- In-memory index cap: env `MAGNITE_RAG_INDEX_MB`, default `512`. Float16 or int8 in memory, float32 on disk.
- Embedders load in half precision (Apple M1, 8 GB). **Default embedder (decided from Task 0, 2026-11-18): chosen by hardware.** `>= 16 GB` RAM or a CUDA GPU gives BAAI/bge-m3; otherwise all-MiniLM-L6-v2 ("Light"). Qwen3-Embedding-0.6B is an optional preset, not a default. Evidence (47 questions, 24k-word handbook PDF): BGE-M3 recall@1 0.68 / MRR 0.78 / handbook recall@1 0.83; MiniLM 0.51 / 0.67 / 0.47; Qwen3 0.53 / 0.68 / 0.57. Measured footprint on the 8 GB M1: MiniLM 435 MB on CPU (41 chunks/s; the GPU adds ~1 GB for no speed gain), BGE-M3 and Qwen3 about 2.9-3.3 GB on the GPU at ~2 chunks/s (Qwen3 on CPU in bf16: 0.7 GB but 0.2 chunks/s). **One embedder model is loaded at a time**; switching unloads and frees the old one before the new one loads.
- **Memory safety (incident 2026-11-18):** an unguarded embedding run on this 8 GB Mac hit an ~11 GB footprint, swapped, and caused a watchdog kernel panic and reboot. Every embedding loop (eval harness, ingest, reindex, query) must use `backend/src/backend/rag/memguard.py`: preflight free-memory check before loading a model, MPS allocator cap, a per-batch `MemoryGuard.check()` (process footprint, system free memory, swap growth), accelerator cache release between batches, small batches. A trip stops the work with a clear error and never takes the system down. Limits: env `MAGNITE_RAG_MAX_MB` (default 35% of RAM, max 8 GB), `MAGNITE_RAG_MIN_FREE_MB` (400), `MAGNITE_RAG_MAX_SWAP_MB` (1024). Peak memory is reported as the real footprint, not `ru_maxrss` (which misses GPU memory).
- Run the offline suite after every task: `PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests -v`
- Commits: one per task, one-line subject, at most 1-2 body lines, no Co-Authored-By trailer, no Claude attribution. Never push, open a PR, or dispatch Actions without asking.
- Ask the user before downloading any model, and ask for the evaluation documents and questions (Task 0).

## Review Focus

Failure modes the spec implies but the main tests would not cover. Each has a pinning test in the owning task.

1. A memory-hungry embedder or a large batch must be stopped by the guard with a clear 503/exit, never by the OS (Tasks 0, 4, 5, 6).
2. Changing the default embedder must not hide an existing library: `search` filters on `d.model = cfg.model`, so a user with MiniLM documents and no saved config would silently get "no results" (Task 4).
3. A warm index must never return a deleted document's chunk; `document_ids: []` must return nothing, not everything (Task 1).
4. A budget where not even the top passage fits must trim that passage at a boundary or raise "No room", never send a 1-character system message (Task 2).
5. Text with no sentence punctuation (code, CJK, one 10k-word line) must still chunk, terminate, and respect the size limit (Task 3).
6. A reindex that fails, is cancelled, or changes dimension midway must leave documents, vectors and config untouched; delete/ingest during a reindex must not corrupt it (Task 5).

## File Structure

| File | Responsibility |
| --- | --- |
| `backend/evals/rag_eval.py` (new) | Task 0 CLI and pure metric functions. Not shipped in the app. |
| `backend/src/backend/rag/memguard.py` (new, Task 0) | Memory limits, preflight, `MemoryGuard`, MPS cap, accelerator cache release. Shared by harness and product. |
| `backend/src/backend/rag/embeddings.py` | `EmbedderSpec`, `PRESETS`, `Embedder`, registry with idle unload; module functions kept for compatibility. |
| `backend/src/backend/rag/store.py` | `_SearchIndex`, search, `reindex`, default-model fallback, reindex guard. |
| `backend/src/backend/rag/context.py` | `retrieve_chunks`, `pack_context`, `injectable_context` wrapper. |
| `backend/src/backend/rag/chunking.py` | Boundary-aware `chunk_text`, `fit_chunking`. |
| `backend/src/backend/rag/batching.py` (new) | `embed_batches` loop with cancel and progress. |
| `backend/src/backend/rag/activity.py` (new) | Single active-operation progress record. |
| `backend/src/backend/rag/schemas.py` | Additive config key `truncate_dim`; words wording; default chunk sizes. |
| `backend/src/backend/routes/rag.py`, `routes/inference.py` | Route wiring; `_with_rag_context` uses packing. |
| `backend/tests/test_rag_*.py` (new) | One test file per task. |

---

### Task 0: Evaluation harness

Runs candidate embedders on the user's own documents on this Mac and reports which should be the default. Output goes to the terminal and a results file outside the repo; no results or user documents are committed.

**Files:**
- Create: `backend/evals/rag_eval.py`, `backend/evals/README.md`, `backend/tests/test_rag_eval.py`, `backend/src/backend/rag/memguard.py`, `backend/tests/test_rag_memguard.py`

**Interfaces:**
- Produces (pure functions, no ML imports):
  - `load_questions(path: str) -> list[Question]`, JSONL, one object per line: `{"question": str, "doc": str, "answer_contains": [str, ...]}`. `Question` is a frozen dataclass with those fields.
  - `is_hit(chunk_text: str, answer_contains: list[str]) -> bool`: true when every needle appears in the chunk, case-insensitive, whitespace-normalized. Chunking-independent ground truth.
  - `recall_at_k(ranked_hits: list[list[bool]], k: int) -> float` and `mrr(ranked_hits: list[list[bool]]) -> float`.
  - `format_report(rows: list[CandidateResult]) -> str` (markdown table: model, dim, chunk words, chunks, recall@1/4/10, MRR, ingest chunks/s, peak RSS MB).
- CLI: `python backend/evals/rag_eval.py --docs <dir> --questions <jsonl> --candidate <hf id> [--chunk-words N] [--dtype float16|bfloat16|float32] [--out results.md]`. Each candidate runs in its own subprocess (`--candidate` runs one; a `--all` wrapper spawns them) so peak RSS (`resource.getrusage`, bytes on macOS) is clean. It uses `chunk_text` and the candidate's query/document prompts from a table in the script (Task 4 later reuses those prompts as presets). It also reports, per document, extraction character count so poor PDF extraction is visible.

- [ ] **Step 1: Write failing tests** in `test_rag_eval.py` (adds `backend/evals` to `sys.path`): `test_is_hit_normalizes_case_and_whitespace`, `test_is_hit_requires_all_needles`, `test_recall_at_k_and_mrr_known_values` (e.g. hits `[[F,T],[T,F],[F,F]]` give recall@1 = 1/3, recall@2 = 2/3, MRR = (1/2+1+0)/3), `test_load_questions_rejects_missing_fields`, `test_format_report_has_one_row_per_candidate`.
- [ ] **Step 2: Run** `... unittest backend.tests.test_rag_eval -v`. Expected: FAIL (module missing).
- [ ] **Step 3: Implement** the pure functions and the CLI. Encoding in the CLI goes through `sentence_transformers` imported inside the candidate subprocess only.
- [ ] **Step 4: Run** the new tests and the full suite. Expected: PASS.
- [ ] **Step 5: Commit** `git commit -m "Add RAG embedder evaluation harness"`
- [ ] **Step 6 (gate, user input):** Ask the user for (a) the documents (paths) and 30-50 questions with the phrase each answer must contain, or offer to draft questions for them to approve, and (b) permission to download Qwen3-Embedding-0.6B (~1.2 GB), BGE-M3 (~2.3 GB) and MiniLM (~90 MB). Do not download before the answer.
- [ ] **Step 5b: Memory guard** (added after the incident above): `memguard.py` with `MemoryLimits`, `MemoryGuardTripped`, `preflight(required_mb, available_mb=None, headroom=1.5)`, `MemoryGuard(limits).check()/.watch(on_trip)/.peak_footprint_mb`, `limits_from_env()`, `apply_mps_limits()`, `release_accelerator_memory()`; the harness uses it (small batches of 4, exit code 75 on a trip). Tested in `test_rag_memguard.py`, including a real-allocation footprint test. Fold into the Task 0 commit.
- [ ] **Step 7:** Run all three candidates in half precision (and MiniLM in float32), compare float16 vs bfloat16 on this Mac for the winner, write results to the scratchpad, and report to the user: recall@k, ingest speed, peak RAM. Output is the default-embedder decision for Task 4. Nothing is committed in this step.

### Task 1: In-memory search index

Today `RagStore.search()` selects every chunk including its text and rebuilds the NumPy matrix on every query.

**Files:**
- Modify: `backend/src/backend/rag/store.py` (`search`, `add_document`, `delete_document`, `clear`)
- Create: `backend/tests/test_rag_search_index.py`

**Interfaces:**
- Consumes: existing `RagStore.search(query_vec: np.ndarray, model: str, top_k: int = 4, min_score: float = 0.0, document_ids: Optional[list[str]] = None) -> list[RagChunk]` (signature unchanged).
- Produces (private, inside `store.py`): `_SearchIndex(chunk_ids: np.ndarray[int64], doc_codes: np.ndarray[int32], doc_table: list[str], matrix: np.ndarray[float16|int8, (N, D)], scale: float)` with `top_candidates(query: np.ndarray, mask: Optional[np.ndarray], n: int) -> np.ndarray` (row indices by approximate score) and `doc_mask(document_ids: list[str]) -> np.ndarray[bool]`; `RagStore._index_for(model: str) -> Optional[_SearchIndex]` (None when over budget); `RagStore._hydrate(chunk_ids: list[int]) -> list[sqlite3.Row]` (text, name, index, tokens, float32 embedding for those ids only); `RagStore._invalidate() -> None`.
- The index holds no chunk text. Scoring converts matrix blocks of 8192 rows to float32 before the dot product (NumPy float16 matmul is slow). Candidates are over-selected (`max(2 * top_k, top_k + 16)`), then rescored in float32 from the hydrated disk vectors, so returned scores and order are exact.
- Env: `MAGNITE_RAG_INDEX_MB` (default 512) and `MAGNITE_RAG_INDEX_DTYPE` (`float16` default, `int8`, `float32`).

- [ ] **Step 1: Write failing tests** (fake normalized vectors, temp `RagStore`):
  - `test_matches_float32_oracle` for dtype `float32` (exact order and `round(score, 4)`), and for `float16` and `int8` (same returned set as the oracle on a seeded 500-chunk fixture, scores exact), across `top_k` in {1, 4, 20} and `min_score` in {0, 0.2}.
  - `test_delete_invalidates_warm_cache`, `test_add_document_invalidates`, `test_clear_returns_empty` (Review Focus 2).
  - `test_empty_document_ids_returns_nothing`, `test_document_ids_filter_matches_oracle`.
  - `test_top_k_larger_than_corpus`, `test_other_model_rows_ignored`.
  - `test_only_top_candidates_are_hydrated`: spy on `_hydrate`, assert it receives at most `max(2*top_k, top_k+16)` ids.
  - `test_over_budget_falls_back_to_batched_scan` with a 1 MB cap: same results as the oracle, `_index_for` returns None.
  - `test_query_dim_mismatch_raises_value_error` (clear message, not a NumPy broadcast error).
- [ ] **Step 2: Run** `... unittest backend.tests.test_rag_search_index -v`. Expected: FAIL.
- [ ] **Step 3: Implement** the index, cache per model guarded by `self._lock`, `_invalidate()` called inside the lock at the end of each write, and the over-budget fallback (stream rows from SQLite in 10k batches, keep a running top-n). Build allocates `(N, D)` once from `COUNT(*)` and fills in batches.
- [ ] **Step 4: Run** the new tests and the full suite. Expected: PASS.
- [ ] **Step 5: Benchmark** in the scratchpad (not committed): 50k chunks x 1024 dims, median of 20 queries, `dev` vs this branch for float32 / float16 / int8, plus index MB. Report numbers; after must not be slower than before at float16.
- [ ] **Step 6: Commit** `git commit -m "Cache a compact in-memory index for RAG search"`

### Task 2: Whole-passage context packing

**Files:**
- Modify: `backend/src/backend/rag/context.py`, `backend/src/backend/routes/inference.py` (`_with_rag_context`, lines 40-72)
- Create: `backend/tests/test_rag_context_packing.py`

**Interfaces:**
- Consumes: `RagStore.search` (Task 1); `engine.count_messages(list[ChatMessage]) -> int`.
- Produces in `context.py`:
  - `retrieve_chunks(query: str, top_k: Optional[int], min_score: Optional[float], document_ids: Optional[list[str]]) -> list[RagChunk]`: the retrieval and error half of today's `injectable_context`, same error messages.
  - `pack_context(chunks: list[RagChunk], fits: Callable[[str], bool], min_chars: int = 128) -> tuple[str, list[RagChunk]]`: header first, then whole passages `"\n\n[i] (from NAME) TEXT"` in rank order while `fits(candidate)`; stop at the first that does not fit. If the **first** passage does not fit whole, trim its text at the last sentence end, else the last whitespace, that fits; the returned chunk's `text` is the trimmed text. Result shorter than `min_chars` raises `ValueError("No room for retrieved context. Start a new chat or reduce max tokens.")`.
  - `injectable_context(...)` stays as a thin wrapper (existing callers and tests).

- [ ] **Step 1: Write failing tests** with a fake `fits` bounding character length: `test_includes_whole_passages_only` (three 300-char chunks, budget 2.5 passages gives exactly two); `test_first_passage_trimmed_at_sentence_boundary` (returned chunk text equals what was sent); `test_no_room_raises` (Review Focus 3); `test_numbering_is_contiguous`; route-level `test_with_rag_context_citations_match_packed_text` calling `_with_rag_context` with a stub engine (`count_messages=lambda m: sum(len(x.content) for x in m)`); citation dict keys stay `document_name`, `index`, `text`.
- [ ] **Step 2: Run.** Expected: FAIL (`pack_context` undefined).
- [ ] **Step 3: Implement** the three functions and rewrite `_with_rag_context` to use them; remove the binary-search loop and offset bookkeeping.
- [ ] **Step 4: Run** the new tests and the full suite; update existing tests that assert mid-passage truncation (`test_regressions.py:193`, `:230` patch `injectable_context`).
- [ ] **Step 5: Commit** `git commit -m "Pack RAG context by whole passages"`

### Task 3: Boundary-aware chunking sized from the model's limit

**Files:**
- Modify: `backend/src/backend/rag/chunking.py`, `backend/src/backend/rag/schemas.py`
- Create: `backend/tests/test_rag_chunking.py`

**Interfaces:**
- `chunk_text(text: str, chunk_size: int = 512, chunk_overlap: int = 64) -> list[str]`: signature unchanged; sizes remain whitespace-split **word** counts. Paragraphs (blank line), then sentences (`.?!` plus whitespace; newline for line-based text); greedily pack up to `chunk_size` words; next chunk starts with trailing whole sentences totalling at most `chunk_overlap` words; a sentence longer than `chunk_size` is hard-split by words with step `chunk_size - chunk_overlap`. Invariants: no chunk exceeds `chunk_size` words; every input word appears in some chunk; existing `ValueError` kept.
- New `TOKENS_PER_WORD = 1.7` (measured 1.3-1.65; conservative) and `fit_chunking(chunk_size: int, chunk_overlap: int, max_seq_tokens: Optional[int]) -> tuple[int, int]`: limit words `= floor((max_seq_tokens - 2) / TOKENS_PER_WORD)`; if `chunk_size` exceeds it, return `(limit, min(chunk_overlap, limit // 4))`; `None` limit returns the inputs unchanged. Applied at ingest (Task 4 provides the limit).
- `RagConfig` defaults change to `chunk_size=320`, `chunk_overlap=40` (about 512 tokens, a good unit for small chat contexts and for 32k-context embedders); stored configs keep their values and are clamped at ingest. Descriptions say "words". Field names and bounds are unchanged.

- [ ] **Step 1: Write failing tests:** `test_no_chunk_exceeds_size` and `test_every_word_covered` on prose, 10k punctuation-free words, spaceless CJK, and a code block (Review Focus 4); `test_prose_chunks_end_on_sentence_boundary`; `test_overlap_is_whole_sentences_within_budget`; `test_empty_and_whitespace_input_returns_empty_list`; `test_invalid_overlap_raises_value_error`; `test_fit_chunking_minilm` (256 tokens gives `(149, 37)` from `(512, 64)`); `test_fit_chunking_noop_when_it_fits` and `test_fit_chunking_none_limit`; `test_schema_descriptions_say_words`; `test_default_config_chunk_sizes` (320/40).
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement** the splitter/packer, `fit_chunking`, schema changes. Existing documents are not rechunked.
- [ ] **Step 4: Run** the new tests and the full suite (fix any test that asserted 512/64 defaults).
- [ ] **Step 5: Commit** `git commit -m "Chunk RAG documents on sentence boundaries"`

### Task 4: Embedder abstraction

Replaces module-level `embed_texts` with an `Embedder`. Default model is set at the last step from Task 0's result.

**Files:**
- Modify: `backend/src/backend/rag/embeddings.py`, `backend/src/backend/rag/store.py` (`get_config` model fallback), `backend/src/backend/rag/schemas.py` (`truncate_dim`), `backend/src/backend/rag/context.py`, `backend/src/backend/routes/rag.py`
- Create: `backend/tests/test_rag_embedder.py`

**Interfaces:**
- `EmbedderSpec` (frozen dataclass): `model_id: str`, `label: str`, `max_seq_tokens: int`, `dim: int`, `query_prompt: str = ""`, `document_prompt: str = ""`, `approx_ram_mb: int`, `trust_remote_code: bool = False`. `PRESETS: dict[str, EmbedderSpec]` keyed by `model_id` with Qwen3-Embedding-0.6B (Qwen "Instruct: ...\nQuery:" query prompt, 32768 tokens, 1024), BGE-M3 (no prompts, 8192, 1024), and all-MiniLM-L6-v2 labelled "Light" (256, 384). Prompt text copied from the model cards. `spec_for(model_id: str) -> EmbedderSpec`: preset, else a generic spec (no prompts, limits read from the loaded model).
- `Embedder(spec: EmbedderSpec, truncate_dim: int = 0)`: `embed_documents(texts: list[str], batch_size: int = 16) -> np.ndarray`, `embed_query(text: str) -> np.ndarray` (1-D), both float32, MRL-truncated to `truncate_dim` when non-zero and then L2-renormalized; `max_seq_tokens: int`, `dim: int` (effective), `count_tokens(texts: list[str]) -> list[int]`, `unload() -> None`.
- `_load_encoder(spec: EmbedderSpec, dtype: str) -> object` is the only place `sentence_transformers` is imported; tests patch it with a fake exposing `encode(...)`, `max_seq_length`, `tokenizer`. `pick_dtype(device: str, torch_module) -> str` is pure: cuda -> bfloat16 if supported else float16; mps -> value measured in Task 0 (float16 unless bfloat16 wins); cpu -> float32. Env override `MAGNITE_RAG_DTYPE`.
- Defaults and device: `default_model(total_mb: float, cuda: bool) -> str` (`>= 16384` MB or cuda gives `BAAI/bge-m3`, else `sentence-transformers/all-MiniLM-L6-v2`), used for `RagConfig.model` only when nothing is stored and no documents exist. `pick_device(spec: EmbedderSpec, total_mb: float, torch_module) -> str`: cuda if available; mps only when `spec.approx_ram_mb >= 1500` and `total_mb >= 16384`; otherwise cpu (the GPU costs ~1 GB fixed and was not faster for small models); env `MAGNITE_RAG_DEVICE` overrides. Preset `approx_ram_mb` values (measured, guard footprint): MiniLM 450, BGE-M3 3000, Qwen3-Embedding-0.6B 3000.
- One model at a time: `get_embedder` first unloads any other loaded embedder (`unload()`, `release_accelerator_memory()`), then runs the preflight, then loads; it never holds two.
- Memory safety: `_load_encoder` calls `memguard.apply_mps_limits()` before importing sentence-transformers (effective only if torch is not yet imported; documented), `memguard.preflight(spec.approx_ram_mb)` before loading (a `MemoryGuardTripped` becomes HTTP 503 with the message), and `embed_documents` runs batches of at most 8 with `guard.check()` and `release_accelerator_memory()` between batches.
- Registry: `get_embedder(model: str, truncate_dim: int = 0) -> Embedder` (one loaded at a time, thread-safe); `unload_if_idle(now: Optional[float] = None) -> bool`; idle timeout env `MAGNITE_RAG_IDLE_UNLOAD_S` (default 300), enforced by a daemon timer re-armed on each use; unload calls `gc.collect()` and `torch.mps.empty_cache()`/`torch.cuda.empty_cache()` when torch is already imported. A later call reloads transparently.
- Compatibility: `is_available()`, `dimension(model)` and `embed_texts(texts, model)` stay (the last delegates to `embed_documents`). Routes call `embed_documents`/`embed_query` through the registry. Existing tests that patch `embed_texts` (`test_october_fixes.py:97`, `test_break_fixes_2026_11.py:775`, `test_regressions.py:180-221`, `:193` on `rag_context`) are updated to patch the new functions.
- `RagConfig.truncate_dim: int = Field(0, ge=0, le=4096)` (0 = full), additive key. `GET /rag/status` gains `embedder: {model, max_seq_tokens, dim, loaded, dtype}` and `chunking: {chunk_size, chunk_overlap, clamped: bool}` (effective values via `fit_chunking`). New `GET /rag/embedders` lists `PRESETS` as JSON. `_ingest_document` applies `fit_chunking` with the embedder's limit and logs a warning (`backend.rag`) when `count_tokens` shows any chunk over the limit.
- `RagStore.get_config()`: when no `model` key is stored and documents exist, return the model of the existing documents instead of the default (Review Focus 1).

- [ ] **Step 1: Write failing tests** with the fake encoder: `test_query_and_document_prompts_applied` (fake records inputs; Qwen preset prefixes queries only; MiniLM prefixes nothing); `test_truncate_dim_slices_and_renormalizes` (norms ~1, dim 512); `test_embedder_never_returns_nan_for_empty_string`; `test_pick_dtype_per_device` with a stub torch; `test_switching_models_never_holds_two` (fake loader counts live encoders; max is 1 and the old one is unloaded before the new load); `test_default_model_by_ram` (8192 MB gives MiniLM; 16384 MB or cuda gives BGE-M3); `test_pick_device_policy` (8 GB + mps stays cpu; cuda wins; env override); `test_preflight_refuses_load_when_memory_is_low` (patched `available_memory_mb`; loader never called; route returns 503); `test_idle_unload_frees_and_reloads` (`unload_if_idle(now=t+301)` returns True, loader called again on next use; not unloaded at `t+299`); `test_status_reports_embedder_and_chunking` (with MiniLM limit 256 and stored 512 words `clamped` is true); `test_ingest_clamps_chunks_to_model_limit`; `test_embedders_route_lists_presets`; `test_default_model_follows_existing_documents` (Review Focus 1); `test_truncate_dim_change_with_documents_is_blocked_by_config` (409, same guard as model).
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement** the module, registry, schema key, store fallback, status/route changes, and migrate callers and existing tests.
- [ ] **Step 4: Run** the new tests and the full suite. Expected: PASS.
- [ ] **Step 5: Wire `default_model`** into `RagStore.get_config` (no stored model and no documents) and `RagConfig`'s static default stays `all-MiniLM-L6-v2` for schema compatibility. Preset labels: BGE-M3 "Quality", MiniLM "Light", Qwen3 "Alternative".
- [ ] **Step 6: Commit** `git commit -m "Add Embedder abstraction with presets and idle unload"`

### Task 5: Reindex

Moves an existing library to a new model or dimension by re-embedding stored chunk text; original files are not needed.

**Files:**
- Create: `backend/src/backend/rag/batching.py`, `backend/tests/test_rag_reindex.py`
- Modify: `backend/src/backend/rag/store.py`, `backend/src/backend/rag/schemas.py`, `backend/src/backend/routes/rag.py`, `docs/LOCAL_API.md`

**Interfaces:**
- `batching.embed_batches(batches: Iterable[list[str]], embed_fn: Callable[[list[str]], np.ndarray], write: Callable[[int, np.ndarray], None], cancelled: Optional[threading.Event] = None, progress: Optional[Callable[[int], None]] = None) -> int`: for each batch checks `cancelled` first (raises `IngestCancelled(RuntimeError)`), runs `guard.check()` (optional `guard: Optional[MemoryGuard]` parameter; a `MemoryGuardTripped` propagates and the route maps it to 503), embeds, calls `write(offset, vectors)`, then `progress(done_so_far)`. Returns total texts embedded. Used again in Task 6.
- `RagStore.reindex(model: str, truncate_dim: int, embed_fn: Callable[[list[str]], np.ndarray], cancelled: Optional[threading.Event] = None, progress: Optional[Callable[[int], None]] = None, batch: int = 64) -> RagConfig`: sets a reindex flag (`add_document`, `delete_document`, `clear` raise `ValueError("Reindex in progress")`; a second reindex too); embeds all chunk texts in id order into a temporary float32 `np.lib.format.open_memmap` file in the rag directory (bounded memory); then one transaction rewrites every `rag_chunks.embedding`, sets every `rag_documents.model`, and stores `model` and `truncate_dim` in `rag_config`; rolls back on any error; deletes the temp file and clears the flag in `finally`; calls `_invalidate()`. Stale temp files are removed in `RagStore.__init__`. Search keeps using the old index until the commit.
- `POST /rag/reindex` body `RagReindexRequest(model: str, truncate_dim: int = 0)` returns `RagConfig`. Errors: 400 empty model, 409 reindex/ingest conflict, 503 embeddings unavailable or embed failure, 499 on disconnect (via `request_work`). Same model and dim is a 200 no-op.

- [ ] **Step 1: Write failing tests** (fake `embed_fn` with configurable dim): `test_embed_batches_checks_cancel_before_each_batch`; `test_embed_batches_stops_when_guard_trips` (fake guard trips on batch 3: write called twice, error propagates, nothing applied); `test_reindex_changes_model_dim_and_search` (384 to 768); `test_failure_midway_leaves_index_untouched` (raise on the 3rd batch; document models, config, vectors and old-model search unchanged) (Review Focus 5); `test_cancel_leaves_index_untouched`; `test_failure_during_apply_rolls_back` (inject an error mid-transaction); `test_writes_blocked_during_reindex` (add, delete, clear raise); `test_temp_file_removed_on_success_failure_and_cancel`; `test_stale_temp_file_cleaned_on_startup`; `test_reindex_same_model_is_noop`; `test_route_reindex_happy_path_and_empty_model_400`; `test_set_config_model_change_still_blocked_with_documents` (existing guard).
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement** `batching.py`, `RagStore.reindex`, route, and the `LOCAL_API.md` entry (also document `GET /rag/embedders` and the new status keys from Task 4).
- [ ] **Step 4: Run** the new tests and the full suite. Expected: PASS.
- [ ] **Step 5: Run the opt-in end-to-end check** `MAGNITE_SELFTEST_RAG=1 ./build/magnite-app --selftest` (ask before it downloads a model; needs a built app). Expected: `[selftest] PASS (0 failures)`.
- [ ] **Step 6: Commit** `git commit -m "Add RAG reindex to switch embedding models"`

### Task 6: Large-document ingest

Fixes the whole-file, one-call, uncancellable ingest.

**Files:**
- Create: `backend/src/backend/rag/activity.py`, `backend/tests/test_rag_large_ingest.py`
- Modify: `backend/src/backend/routes/rag.py` (`_ingest_document`, `reindex` route, new route), `backend/src/backend/rag/store.py` (only if `reindex` needs the progress hook)

**Interfaces:**
- `activity.Activity`: `start(kind: Literal["ingest", "reindex"], name: str, total: int = 0) -> None`, `set_phase(phase: str, total: Optional[int] = None) -> None`, `advance(done: int) -> None`, `finish() -> None`, `snapshot() -> dict` with `{active: bool, kind, name, phase, done, total}`; module singleton `get_activity()`. Phases: `extracting`, `chunking`, `embedding`, `saving`.
- `GET /rag/activity` returns `snapshot()` (additive; the C++ client ignores it).
- `_ingest_document` runs: extract, chunk, `embed_batches` with batch size `MAGNITE_RAG_BATCH` (default 16) into a preallocated float32 `(N, D)` matrix (allocated on the first batch; worst case at the 16M-character cap and 64-word chunks is stated in the docs), progress on each batch, `cancelled` checked before each batch, then one atomic `add_document` (unchanged). Ingests are serialized by a lock that polls `cancelled` while waiting, so queued requests keep today's behaviour and a cancelled waiter leaves immediately; `reindex` (Task 5) takes the same lock.
- Cancel: client disconnect sets `cancelled`; the next batch boundary raises `IngestCancelled`; nothing is stored. Mid-ingest failure leaves no document or chunks.

- [ ] **Step 1: Write failing tests** (fake embedder counting calls and batch sizes): `test_20k_chunks_embedded_in_batches_of_at_most_16`; `test_cancel_after_second_batch_stops_and_stores_nothing` (embed called exactly twice, `list_documents()` empty); `test_progress_reaches_total_then_clears` (activity snapshot `done == total` at the end of embedding, `active` false after); `test_failure_midway_stores_nothing`; `test_second_ingest_waits_for_first` and `test_cancelled_waiter_leaves_immediately`; `test_ingest_during_reindex_returns_409`; `test_activity_route_reports_active_ingest`; `test_atomic_document_insert_preserved`.
- [ ] **Step 2: Run.** Expected: FAIL.
- [ ] **Step 3: Implement** `activity.py`, wire both routes, add `GET /rag/activity`.
- [ ] **Step 4: Run** the new tests and the full suite. Expected: PASS.
- [ ] **Step 5: Scale check** in the scratchpad (not committed): ingest a ~1,000-page synthetic text with the chosen embedder on this Mac; record time, peak RSS, and that cancel mid-way returns promptly. Report numbers.
- [ ] **Step 6: Commit** `git commit -m "Ingest large documents in cancellable batches with progress"`

### Task 7: Documentation sync

**Files:**
- Modify: `docs/development/architecture.md`, `docs/LIMITATIONS.md`, `docs/LOCAL_API.md`

- [ ] **Step 1: Update `architecture.md`:** the RAG paragraph (~line 343) and "Retrieval and chat augmentation" (~lines 355-365) for the Embedder, compact cached index and its memory cap, whole-passage packing, the structured `sources` SSE event and `document_ids` scoping (already shipped, currently undocumented), reindex, batched ingest and activity; fix the `RagPanel` row (~line 131) for Knowledge living in the Conversation workspace; update the "Current architectural limits" sentence (~line 474: "flat full-scan retrieval" becomes "in-memory exact scan, no ANN index").
- [ ] **Step 2: Update `LIMITATIONS.md`** (lines 46-48): remove the missing-reindex and words/tokens caveats, keep PDF OCR, add the ~300k-chunk exact-scan ceiling, the 512 MB index cap, and that the C++ Knowledge panel has no reindex or progress UI yet.
- [ ] **Step 3: Verify** every named function, route and env var in the edited docs with `grep`, and that links resolve. Confirm `LOCAL_API.md` lists every new route (`/rag/embedders`, `/rag/reindex`, `/rag/activity`) and key.
- [ ] **Step 4: Commit** `git commit -m "Update RAG docs for the engine changes"`

## Self-Review

- **Coverage:** Task 0 evaluation, 1 index, 2 packing, 3 chunking, 4 Embedder, 5 reindex, 6 large ingest, 7 docs: every Phase A item in the review's section 7. Review findings 1-5 map to Tasks 4+3, 6, 1, 2+3, 1.
- **Types:** `embed_batches` (Task 5) is consumed by Task 6 with the same signature; `fit_chunking` (Task 3) is consumed by Task 4; `_invalidate` (Task 1) is called by Task 5; `RagConfig.truncate_dim` (Task 4) is passed through `RagStore.reindex` (Task 5).
- **Deviations from the old plan:** one added risk found while planning: a default-model change would hide existing libraries (Task 4 fallback). Reindex stages vectors in a memmap file instead of an in-memory dict, to stay within the memory bound.
- **Not in Phase A:** hybrid search, reranking, GGUF embedder, Docling/OCR, C++ UI for presets, reindex, and progress, rechunking existing documents.

## Open Questions

None blocking. Two things Task 0 will settle: the default embedder, and whether `float16` or `bfloat16` is the MPS default.

## As built (2026-11-18)

All of Phase A shipped on branch `rag-engine-update`, one commit per task. Deviations from this plan:

- **Memory guard (not in the original plan).** An unguarded eval run panicked the 8 GB Mac, so `rag/memguard.py` is used by every embedding loop (Tasks 0, 4, 5, 6).
- **Default embedder** is chosen by hardware (`>= 15000` MB RAM or CUDA gives BGE-M3, else MiniLM), not a single model. The threshold is 15,000 MB, not 16,384, so a "16 GB" Linux box counts as big.
- **Device policy:** CPU unless CUDA, or the heavy presets on a 16 GB+ Apple machine. Measured: the Apple GPU costs ~1 GB fixed and was not faster for MiniLM.
- **Search index:** `_SearchIndex.top_candidates` returns row indices as planned; scoring converts 8,192-row blocks to float32; exact float32 rescoring of the over-selected candidates.
- **Chunking:** CJK characters and tokens over 24 characters are weighted; after the first batch loads the model, real token counts trigger re-chunking (up to 3 times) because number-heavy text measured ~2.3 tokens/word against the 1.7 estimate.
- **Reindex** takes `truncate_dim: int = 0` and refuses unsupported truncation with 400; staging is an on-disk memmap.
- **Ingest/reindex are serialized** (queued, cancellable while queued) and `GET /rag/activity` reports progress.
- **UI contract (requirement, 2026-11-18):** whatever model is used, the Knowledge panel must display the parameters the backend is using (`GET /rag/status` `embedder` and effective `chunking`). Documented in architecture.md, guarded by `test_rag_ui_contract.py`; the Knowledge panel now follows it (branch `rag-knowledge-ui`), with a model picker, reindex, progress and speed hints.
- **Not done (Phase B/C):** hybrid FTS5 search, reranker, GGUF embedding backend, Docling.
