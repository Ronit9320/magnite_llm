# RAG Engine Break Campaign Plan

> **For agentic workers:** Execute inline (superpowers:executing-plans). Steps use checkbox syntax.

**Goal:** Find every way the RAG engine (ingest, chunking, embeddings, SQLite/FTS5 store, hybrid search, context packing, Knowledge UI contract) returns wrong results, crashes, hangs, leaks memory or loses data, and record each as a reproducible finding.

**Architecture:** A probe harness (`docs/testing/rag_break/`, git-ignored like the earlier `break_recheck.py`). Every probe is a function that returns HELD / BROKEN / NOTE with evidence. The orchestrator runs each probe in its own child process (timeout = HUNG, crash = finding), so one hang or OOM never hides the rest. A fake deterministic embedder covers logic and scale; the real MiniLM model covers only what needs a real tokenizer or encoder.

**Tech Stack:** Python 3.13, FastAPI + httpx ASGITransport, SQLite FTS5, numpy, sentence-transformers (MiniLM only).

**Spec:** `docs/development/RAG_ENGINE_UPDATE_PLAN.md`, the Phase A/B plans in `docs/superpowers/plans/`, and the code under `backend/src/backend/rag/` + `routes/rag.py`.

## Global Constraints

- Never touch `~/.magnite`: every probe sets `MAGNITE_HOME` to a scratch dir.
- 8 GB M1: real-model probes use **MiniLM only**, one model per process, through the existing memguard. No BGE-M3 / Qwen3 runs. Scale probes use the fake embedder.
- No commits, no push (standing rules). The harness and the report stay local/uncommitted.
- This campaign **finds and documents**; it does not change product code. Fixes are a follow-up the user approves per finding.
- Do not re-test what existing unit tests already pin (nasty FTS text, min_score-is-cosine, etc.) except as a differential/fuzz oracle.

## Review Focus (what the spec is silent on and users will hit)

1. Files that are not text (docx, png, UTF-16, huge) named `.txt`: should be rejected or handled, not silently embedded as garbage.
2. Same document ingested twice, or near-duplicate chunks: top-k should not be filled by copies.
3. Slow machines: a batch longer than the idle-unload timer, queries during an ingest/reindex.
4. Large libraries: query latency, lock hold time blocking the UI's status/list calls.
5. Damaged/odd stored data: NaN vectors, truncated blobs, missing FTS table.

## Phases

### Phase 0: Baseline
- [ ] Run the full offline suite, record pass/fail count (baseline for "did my probes change anything").
- [ ] Confirm MiniLM loads under the memguard and note footprint.

### Phase 1: Harness (Task 1)
**Files:** Create `docs/testing/rag_break/{harness.py,run.py}`.
**Interfaces:** `@probe(id, title, group, real_model=False, timeout=120)`; probe returns `held(msg)`, `broken(msg, **evidence)` or `note(msg, **evidence)`; `Env` gives a scratch dir, an `httpx` ASGI client, the store, fake-embedder patching and `fake_vec(text)`.
- [ ] Implement the fake embedder (hashed bag-of-words, dim 64, unit norm) patched over `embeddings.is_available/embed_documents/embed_query`.
- [ ] Orchestrator: `run.py [--only A1,B2] [--group A]`, child process per probe, JSON result per line, final table and `results.json`.
- [ ] Smoke test with two trivial probes (one HELD, one deliberately BROKEN, one hanging) to prove all three outcomes are reported.

### Phase 2: Ingest and extraction probes (Task 2, `p_ingest.py`)
A1 binary/zip bytes as `.txt` · A2 UTF-16 file · A3 path ingest has no size cap (content has 16M) · A4 NUL bytes · A5 lone surrogate in `name`/`kind`/`path` · A6 markdown image syntax · A7 HTML parser worst cases (timing) · A8 chunker pathological timing + property fuzz (size bound, no empty, coverage, overlap) · A9 duplicate document ingest · A10 unbounded `name`, header injection in packed context · A11 HTTP type/shape fuzz on every `/rag` endpoint (never 500) · A12 PDF edge cases (garbage, blank, encrypted, zero pages).

### Phase 3: Store and search probes (Task 3, `p_store.py`)
B1 NaN/inf vectors · B2 truncated blob poisons search and ingest · B3 orphan chunks vs counts · B4 FTS integrity after random add/delete/clear (`integrity-check`) · B5 differential: float16/float32/int8/scan vs exact numpy top-k, hybrid vs brute-force keyword · B6 huge/odd `document_ids` and SQL-ish strings · B7 odd `top_k`/`min_score` through route, blueprint `search()` and chat path · B8 keyword-leg quality: identifiers (`C++`, `v2.0.1`, `E_CONN_RESET`, emails, IPs), CJK substring, accents, stopword-only query · B9 scale: 150k chunks, cold/warm query, stopword OR query, delete/clear time, over-budget scan, lock hold blocking `GET /rag/documents` · B10 SQLite write-lock held by another process · B11 config oddities (truncate on non-MRL model, bogus model id, alias vs full id, model change during ingest).

### Phase 4: Concurrency and lifecycle probes (Task 4, `p_concurrency.py`)
C1 real MiniLM: concurrent embed_query + embed_documents (tokenizer "Already borrowed") · C2 query during reindex thrashes the single-embedder registry · C3 idle-unload timer firing inside a long batch · C4 queue of 30 ingests vs the thread pool (do `/health`, `/rag/activity`, `/rag/query` still answer?) · C5 randomized concurrent add/delete/clear/query/config/reindex stress with invariant checks and a deadlock watchdog · C6 kill -9 during ingest and reindex, reopen, verify integrity and leftover temp files.

### Phase 5: Context, integration and UI contract (Task 5, `p_context.py`)
D1 retrieved text injected as `system` role (indirect prompt injection) · D2 `pack_context` fuzz (fits holds, used chunks present, order, trim) · D3 retrieval query = whole last message (huge/long, follow-up questions) · static read of `app/src/panels/rag*.cpp` against backend responses (499/503/409, long names, polling stop, large lists).

### Phase 6: Triage and report (Task 6)
- [ ] Re-run every BROKEN probe twice to rule out flakes.
- [ ] Write `docs/testing/RAG_BREAK_REPORT_2026-11-20.md`: per finding id, severity (Critical/High/Medium/Low), repro command, evidence, root cause in `file:line`, suggested fix. Plus a "held" list so coverage is visible.
- [ ] Re-run the offline suite; confirm no product file changed (`git status`).
- [ ] Save a project memory entry with how to re-run the harness.

## Done when
Every probe in the catalogue has a recorded outcome, every BROKEN has been reproduced twice, and the report ranks the findings.
