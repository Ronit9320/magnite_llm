# RAG Engine Research Review

Date: 2026-11-17. Input to [RAG_ENGINE_UPDATE_PLAN.md](RAG_ENGINE_UPDATE_PLAN.md). Nothing in the code has changed.

**How to read the evidence.** Three kinds of claims appear below, and they are labelled:
- **[measured]** I ran it on this machine (Apple M1, 8 GB, the repo's `.venv`).
- **[model card]** read from the model's official Hugging Face page.
- **[third-party]** blogs, papers and leaderboards. Scores from different sources use different MTEB variants and are **not comparable with each other**; vendor numbers are self-reported.

---

## 1. What actually fails on large documents

You said the current setup is too small for larger documents. The research says that is true, but the cause is mostly **not** the vector search. Ranked by how much it hurts:

| # | Problem | Evidence | Fixed by |
| --- | --- | --- | --- |
| 1 | **The embedder never sees most of each chunk.** `all-MiniLM-L6-v2` has `max_seq_length = 256` word-pieces [measured]. A 512-word chunk is roughly 660-840 word-pieces [measured: 841 on a number-heavy sample; plain prose is lower], so about 60-70% of every default chunk is silently dropped before it is turned into a vector. Retrieval can only find the first part of each chunk. | local run | A longer-context embedding model, plus chunk sizes tied to the model's limit |
| 2 | **Ingestion is one big blocking call.** The whole file is extracted to memory (limit 16M characters), every chunk is embedded in one `embed_texts` call, and cancellation is only checked once, after all embedding is done (`routes/rag.py`). No progress, no batching. LIMITATIONS.md already lists ingestion progress as unfinished. | code | Batched ingest with progress and per-batch cancel |
| 3 | **Retrieval reloads everything per query**, including the text of every chunk. | code | Plan Task 1 |
| 4 | **Context budget.** Default is 4 chunks x 512 words, then cut by character count. Small local chat models have small context windows, so the answer to "retrieve more" is *smaller, better chunks*, not top-20. | code + [third-party] | Plan Tasks 2 and 3 |
| 5 | **Search speed is not the problem.** Exact NumPy scan [measured]: 100k chunks x 384 dims = 4 ms; 100k x 1024 = 13 ms; 300k x 1024 = 34 ms (1.2 GB of RAM). **Memory is the limit, not time.** | local run | Memory cap and smaller in-memory vectors (section 4) |

**Consequence:** do not add FAISS, an ANN index, or a vector database. Spend the effort on the embedder, chunking, and ingestion. That agrees with the architecture note that a scalable index needs a durable sync strategy: we avoid it entirely.

---

## 2. Embedding models

Constraint set for Magnite: local and offline after first download, one-click (no login), permissive license for a distributed app, works with `sentence-transformers` (already pinned `>=2.2.0`; installed is 6.1.0, transformers 5.18.0 [measured]), and fits next to a chat model on an 8 GB machine.

| Model | Size / dims | Context | License | Needs query prefix? | Notes |
| --- | --- | --- | --- | --- | --- |
| **all-MiniLM-L6-v2** (current) | 22.7M / 384 [measured] | **256** [measured] | Apache-2.0 | no | Fast (99 chunks/s of 512 words on this Mac [measured]) but old and short-context. |
| **Qwen3-Embedding-0.6B** | 0.6B / 1024, any size 32-1024 (MRL) | 32k | Apache-2.0 | **yes**: `prompt_name="query"`; skipping costs ~1-5% | MTEB English v2 mean 70.70, retrieval 61.83; multilingual mean 64.33. Needs `transformers>=4.51` (ok). [model card] |
| **BGE-M3** | 1024 | 8192 | MIT | **no** | 100+ languages; dense, sparse and multi-vector modes; page recommends hybrid retrieval plus a reranker for RAG. Parameter count not on the card. [model card] |
| **Harrier-OSS-v1-0.6B** (Microsoft) | 0.6B / 1024 | 32k | MIT | yes: `Instruct: {task}\nQuery: {query}` | "MTEB v2" 69.0 (card does not say which v2 suite). Last-token pooling. Newer, so less field-tested. [model card] |
| **Nomic Embed v1.5** | ~0.1B / 768, MRL 64-768 | 8192 | Apache-2.0 | **yes**: `search_query:` / `search_document:` | Currently needs `trust_remote_code=True` (runs downloaded code); the card says that goes away with transformers 5.5 / sentence-transformers 5.3, which we have. [model card] |
| **EmbeddingGemma-300M** | 0.3B / 768, MRL 128-768 | 2048 | **Gemma license, gated** | yes (`task: search result \| query: ...`) | Login and license acceptance on Hugging Face breaks one-click download. fp16 not supported. QAT Q8_0 variant scores 60.93 vs 61.15 full precision (multilingual v2). [model card] |
| **Jina v5 text small** | 0.68B / 1024 | 32k | **CC BY-NC 4.0** (commercial use needs a deal with Jina) | yes | Top English v2 score I saw (71.7) but the license rules it out for a distributed app. `trust_remote_code=True`. [model card] |

Bigger siblings exist (Qwen3-Embedding 4B and 8B lead open leaderboards [third-party]) but do not fit on an 8 GB machine alongside a chat model.

### Memory on an 8 GB Mac (important)

Parameter count x bytes per weight: Qwen3-0.6B in float32 is about **2.4 GB**, in bf16/fp16 about **1.2 GB**, as a Q8 GGUF about **0.65 GB** (my arithmetic, not measured). MiniLM is about 90 MB. A chat model is loaded at the same time, so the embedder must be loaded in half precision, and ideally freed when idle. This is why a model "picker" with sane presets is better than a free-text field.

### Recommendation

1. **Default: Qwen3-Embedding-0.6B**, loaded in bf16, 1024 dims by default with an option to truncate (MRL) to 512 to halve index memory. It has the best published English retrieval number among the small permissive models, a 32k context that removes the truncation problem, and Apache-2.0.
2. **Alternative: BGE-M3** if you prefer no query prefix and the strongest multilingual coverage. It is the lowest-risk drop-in (no prompts). It also keeps the door open for its sparse mode later.
3. **Keep MiniLM as a "Light" preset** for low-RAM machines, with a warning and a smaller default chunk size (it still truncates at 256).
4. **Do not use** Jina v5 (license) or EmbeddingGemma (gated) as defaults.

**I would not trust the leaderboard to pick between #1 and #2.** The plan adds a small evaluation task (section 7, Task 0) that runs the candidates on *your* documents on *this* Mac and reports recall, ingest speed and RAM.

### Two ways to run the model

- **sentence-transformers (current).** Simple, supports prompts and MRL, needs torch (the `[ml]` extra).
- **llama.cpp GGUF via the existing `llama-cpp-python`.** The installed version is 0.3.36 and its `Llama(...)` accepts `embedding=True` and `pooling_type` [measured]. Community GGUF builds of Qwen3-Embedding exist, and the model needs **last-token pooling**; one card warns that wrong pooling gives wrong vectors silently, without an error [third-party]. Benefit: RAG would work in the base install and the packaged app without torch, at about 0.65 GB. Risk: correctness, so it would need a test that compares GGUF vectors against the sentence-transformers ones (cosine close to 1). **Recommend as a later phase**, not the first change.

---

## 3. Retrieval pipeline techniques, ranked by value for cost

| Technique | What it does | Evidence | Verdict |
| --- | --- | --- | --- |
| **Right-sized, boundary-aware chunks** | Chunks end at sentence boundaries and fit the embedder's limit. | In an independent test, plain fixed chunking matched fancier methods [third-party]. | **Do now** (Plan Task 3, size tied to model limit). |
| **Query/document prompts** | Qwen3, Nomic, Harrier and Gemma expect different text for queries vs documents. | Skipping it costs about 1-5% retrieval for Qwen3 [model card]. | **Do now.** Required by any model except BGE-M3 and MiniLM. |
| **Batched, cancellable, progress-reporting ingest** | Embed in batches, report progress, check cancel each batch, insert incrementally. | Code reading; LIMITATIONS.md. | **Do now.** The actual large-document fix. |
| **Hybrid search: keyword (SQLite FTS5) + vectors, merged by Reciprocal Rank Fusion** | Keyword search catches IDs, rare terms and exact phrases that embeddings miss; embeddings catch paraphrases. RRF merges ranks without tuning score scales (`1/(k+rank)`, k=60 typical). | FTS5 and JSON1 are present in this Python's SQLite 3.45.3 [measured; the packaged runtime must be re-checked]. Anthropic reports failure rate 5.7% to 2.9% with contextual embeddings plus BM25 [Anthropic post]. Gains from hybrid alone were anecdotal in what I found. | **Next phase.** Low cost, no new dependency. Adds a derived table to `rag.db` (see open question). |
| **Reranker (cross-encoder on the top ~20-30 candidates)** | Re-scores candidates by reading query and passage together. | Anthropic: 2.9% to 1.9% on top of the above. Small rerankers: MiniLM-L6 23M, mxbai-xsmall 71M, bge-reranker-base 278M; big ones: bge-reranker-v2-m3 568M (MIT), Qwen3-Reranker-0.6B (Apache). Sources disagree on which 0.6B-class model wins; latency numbers vary 14 ms to 200 ms per pair by setup [third-party]. | **Optional, off by default**, after measuring. Memory-heavy on 8 GB. |
| **Parent/neighbour expansion** (return the chunks next to a hit) | Gives the chat model surrounding context. Cheap because `chunk_index` is already stored. | I found no benchmark. | Cheap to try inside the evaluation task; do not promise gains. |
| **Contextual Retrieval** (LLM writes a 50-100 token situating blurb per chunk before embedding) | Fixes chunks that lose meaning out of context. | Anthropic's numbers use Claude with prompt caching. A small independent benchmark on SEC filings and arXiv found no gain over plain chunking [third-party]. | **Defer.** Needs a local LLM call per chunk, which is slow and weak on 8 GB. |
| **Late chunking** (embed the whole long text, then pool per chunk) | Chunks keep document-wide context. | About 2-4% relative nDCG, bigger on long documents [third-party]. Needs long-context embedder and token-level outputs, not a plain `encode()`. | **Defer.** |
| **Full-context mode** | If the whole knowledge base fits in the model's context, skip retrieval. Anthropic's rule of thumb: under ~200k tokens. | Anthropic post. Our local chat models usually have 2k-8k contexts, so this only helps for a single short document. | **Possible small feature:** if one selected document fits the loaded model's budget, inject it whole. Engine already knows `context_size()`. |

---

## 4. Vector storage and scale

- Exact scan is fast enough (section 1) up to hundreds of thousands of chunks. [measured]
- **In-memory size is the limit.** 100k chunks: 154 MB at 384 dims, 410 MB at 1024 dims, 1.2 GB for 300k at 1024 [measured]. On an 8 GB Mac the plan's 512 MB default cap holds about 125k chunks at 1024 dims in float32.
- **Cheap fixes** (no format change): keep float32 on disk, hold **float16 or int8** in the in-memory index (2-4x smaller), and offer MRL truncation to 512 dims at ingest time.
- **Rejected alternatives:**
  - *sqlite-vec*: its own README says it is pre-v1 with breaking changes expected, and the comparisons I found describe it as brute-force; it brings no advantage over NumPy for us. [third-party]
  - *hnswlib*: no built-in persistence [third-party], so we would need the very sync/rebuild strategy architecture.md warns about.
  - *FAISS*: heavy dependency, no benefit below the sizes above.
- Revisit ANN only when real users exceed roughly 300k chunks.

---

## 5. Document extraction (PDF)

Today: `pypdf`, no OCR. Third-party comparisons say pypdf is the weakest on multi-column and table-heavy layouts. PyMuPDF/pymupdf4llm is fast but carries an **AGPL** license, which needs a decision before shipping in a distributed app. Docling runs locally with layout and table models but is heavy. The best tool varies by document type [third-party]. **Recommendation:** keep pypdf in this round; add a small extraction-quality check to the evaluation task so you can see whether your real PDFs need more; evaluate Docling as an opt-in extra later.

---

## 6. Answer to "can it handle larger documents?"

After the "do now" items: yes for a personal library of tens of thousands of chunks, on an 8 GB Mac, provided the embedder is loaded in half precision. A 1,000-page book is a few thousand chunks (about 160-word chunks) and indexes in minutes. Not covered: scanned PDFs (no OCR), tables inside PDFs, and collections beyond roughly 300k chunks.

---

## 7. Proposed revisions to the plan

The current plan is still right about search, packing, chunking and reindex. Research adds items and changes two. Awaiting your approval before I rewrite it.

**Phase A: fix large documents (must have)**
- **Task 0 (new): evaluation harness.** A small script plus a 30-50 question set built from your own documents. Reports recall@k, ingest time and peak RAM for MiniLM, Qwen3-0.6B and BGE-M3 on this Mac. Output decides the default. Not shipped in the app.
- **Task 1:** search index (as planned) with float16/int8 in-memory vectors.
- **Task 2:** whole-passage packing (as planned).
- **Task 3:** chunking (as planned). Default sizes derived from the chosen model's limit, not fixed at 512.
- **Task 4 (replaces "report the token limit"):** an `Embedder` abstraction with presets: `embed_documents`, `embed_query`, `max_seq_tokens`, `dim`, loaded in bf16, with idle unload. Includes query/document prompts and optional MRL truncation.
- **Task 5:** reindex (as planned). This is how an existing library moves to the new model.
- **Task 6 (new): large-document ingest.** Batched embedding, progress, cancel between batches, bounded memory.

**Phase B: better ranking**
- **Task 7 (new):** hybrid FTS5 + RRF.

**Phase C: optional**
- **Task 8:** opt-in reranker. **Task 9:** GGUF embedding backend (torch-free RAG). **Task 10:** Docling extraction extra.
- **Task 11:** docs sync (was Task 6).

## 8. Decisions I need from you

1. Approve Phase A as the next build, with Phase B and C as follow-ups?
2. Default embedder: Qwen3-Embedding-0.6B (my pick) or BGE-M3? Or let the evaluation in Task 0 decide?
3. OK to run Task 0 first? It downloads about 1.2 GB for Qwen3 and about 2.3 GB for BGE-M3, and it needs a handful of real documents with questions you know the answers to.
4. Hybrid search adds an FTS5 table to `rag.db`. It is derived data that can be rebuilt, but AGENTS.md says to bump `DATA_SCHEMA` when persistent formats change and the updater rejects schema changes. Bump or treat as derived?
5. Should the in-memory index default be a hard 512 MB cap (about 125k chunks at 1024 dims in float32, about 250k in float16)?

## Sources

- Qwen3-Embedding-0.6B model card: https://huggingface.co/Qwen/Qwen3-Embedding-0.6B
- BGE-M3 model card: https://huggingface.co/BAAI/bge-m3
- Nomic Embed v1.5 model card: https://huggingface.co/nomic-ai/nomic-embed-text-v1.5
- EmbeddingGemma-300M model card: https://huggingface.co/google/embeddinggemma-300m
- Harrier-OSS-v1-0.6B model card: https://huggingface.co/microsoft/harrier-oss-v1-0.6b
- Jina v5 text small model card: https://huggingface.co/jinaai/jina-embeddings-v5-text-small
- Anthropic, Contextual Retrieval: https://www.anthropic.com/news/contextual-retrieval
- Chunking benchmark (naive vs late vs contextual): https://github.com/Tirthg995/rag-chunking-benchmark
- Embedding model roundup: https://d-central.tech/local-embedding-models/ and https://supermemory.ai/blog/best-open-source-embedding-models-benchmarked-and-ranked/
- Rerankers: https://jina.ai/news/smaller-faster-cheaper-jina-rerankers-turbo-and-tiny and https://arxiv.org/pdf/2605.06132
- Hybrid search with SQLite FTS5 and RRF: https://dev.to/soytuber/building-a-hybrid-rag-in-200-lines-sqlite-fts5-sqlite-vec-rrf-38h1
- sqlite-vec: https://github.com/asg017/sqlite-vec
- Vector search comparison: https://arxiv.org/pdf/2606.19458
- Qwen3-Embedding GGUF and pooling notes: https://huggingface.co/Qwen/Qwen3-Embedding-8B-GGUF
- PDF parsing evaluations: https://arxiv.org/pdf/2604.12047 and https://dev.to/ashokan/from-pdfs-to-markdown-evaluating-document-parsers-for-air-gapped-rag-systems-58eh
