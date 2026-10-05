# Backend audit and verification — 2026-10-05

Scope: all Python modules under `backend/src/backend`, the backend test runners,
backend packaging/CI configuration, and the Qt startup/IPC callers. This is a
source review plus targeted automated and real-model verification, not a claim
that every hardware, model, failure, or concurrency combination has been tested.
The working tree already contained desktop/design changes; this audit did not
edit those changes or rebuild the desktop.

The detailed startup, function calls, execution threads, storage paths, and
shutdown flow are in [BACKEND_PROCESS.md](BACKEND_PROCESS.md).

## Findings fixed in this change

| Finding | Cause | Change and verification |
| --- | --- | --- |
| Existing HF cache models missing from the library | `list_synced()` only refreshed Ollama; HF rows were created by Magnite pulls, not by scanning existing files | Offline Hub scan on model-list refresh, one stable record per repo; synthetic-cache and real-cache tests |
| `HF_HOME` downloads/scans pointed at the wrong directory | Startup passed `HF_HOME` as `cache_dir`, although it is the parent of `hub` | Use the installed Hub library's `HF_HUB_CACHE` default; process-level tests cover modern/legacy overrides, HF_HOME and XDG |
| Training names could escape the adapter directory or overwrite earlier weights | `os.path.join(adapter_dir_root, cfg.name)` interpreted display text as a path; repeated names reused a directory | Save under unique `adapter-<job-id>` directories; exercise the real trainer save/registration path with stubbed ML and verify relative/absolute hostile names and duplicates |
| Unexpected local config shape could break metadata inspection | `_inspect_hf()` assumed decoded JSON was a dictionary | Treat non-object config as missing architecture; cache regression covers it |

Discovery keeps existing metadata and usable selected revisions, prefers `main`
for newly discovered models, skips incomplete indexed weights/adapter-only
snapshots, tolerates a broken repository or a disappearing cache, and avoids
rewriting unchanged SQLite rows. The registry's scan/delete lock prevents those
two operations from interleaving within one registry instance. It is not a
cross-process lock or a complete download/training/deletion coordination system.

## Cache investigation on this workstation

A read-only scan of `~/.cache/huggingface/hub`, with a new temporary registry,
found the following six repositories in approximately 0.02 seconds. Sizes are
snapshot file totals, not deduplicated disk usage or runtime memory needs.

| Repository | Architecture reported locally | Approx. MiB | Verification |
| --- | --- | --- | --- |
| `HuggingFaceTB/SmolLM2-135M` | `LlamaForCausalLM` | 259.8 | Discovery only |
| `aladar/tiny-random-LlamaForCausalLM-GGUF` | No config architecture | 2.7 | Discovery, real llama.cpp load/completion/chat SSE/unload |
| `sshleifer/tiny-gpt2` | `GPT2LMHeadModel` | 4.5 | Discovery plus real torch/PEFT training/inference workflow |
| `microsoft/harrier-oss-v1-0.6b` | `Qwen3Model` | 1152.1 | Discovery only; not evidence of causal-chat compatibility |
| `sentence-transformers/all-MiniLM-L6-v2` | `BertModel` | 87.3 | Discovery and real RAG embedding/retrieval; not a chat model |
| `microsoft/phi-2` | `PhiForCausalLM` | 5305.1 | Discovery and complete shard-index recognition only |

Other cache contents explain why “every directory” is not a loadable base model:

- `venkycs/phi-2-instruct` contains adapter configuration/weights, not standalone
  base weights; it is not auto-imported into the adapter registry. Current adapter
  import expects a Magnite export ZIP, not an arbitrary Hub adapter directory.
- `Qwen/Qwen-Image-2.1` had no recognized base weights in its cached snapshot.
  Image generation also is not supported by these causal-language-model engines.
- `HuggingFaceH4/ultrachat_200k` is a dataset cache, not a model repository.
- `tiny-gpt2` includes an additional partial PR revision. The usable `main`
  snapshot is selected rather than the weights-only partial revision.

No user cache weights were pulled, deleted, or moved by the verification runs.
The real application database was not rewritten during testing: scan checks and
API workflows used temporary application homes. Restart the app to load the code
change, then refresh the local model list to populate its normal registry.

## Verification performed

The initial existing suite passed 24 tests after moving execution outside the
restricted sandbox. Additional tests cover cache discovery and environment
resolution, pull-job success/failure/metadata outage, adapter output ownership,
and operation with ML imports deliberately unavailable.

| Layer | Test / evidence | Result |
| --- | --- | --- |
| Backend regressions | `unittest discover -s backend/tests -v` | **41 tests passed in 6.799 seconds**, exit 0 |
| Filesystem integration | Real Hub snapshot/symlink layouts in temporary caches | Discovery, filtering, revision choice, shards, corruption, missing cache, preservation and deletion pass |
| Route integration | ASGI model-list request → registry row; existing API tests | Pass |
| Missing dependency behavior | Fresh subprocess blocks ML imports; starts FastAPI, checks health/status, rejects RAG, records training error | Included in final suite |
| Real ML workflow | `backend/tests/live_workflow.py`, Hub/Transformers offline | Five workflow checkpoints pass |
| Real backend process | `backend/tests/live_backend_smoke.py` | Four process/HTTP checkpoints pass |
| Actual existing cache | Scan with temporary `models.db` | Six base-weight repositories discovered |
| Syntax / patch hygiene | `compileall` and `git diff --check` | Both passed with no diagnostics |

The real ML workflow exercised registered transformer load, nonstreaming chat,
three-step mixed instruction/text LoRA training, adapter registration/activation,
real embedding ingestion and query, RAG-assisted adapter chat, export/delete/
import, document deletion, and durable job history across FastAPI lifespans.
It uses cached tiny-gpt2 and MiniLM. Test artifacts were created in a temporary
`/tmp/magnite-live-workflow-*` directory; the runner prints the specific location.

The new process smoke runner starts `python -m backend.main --port 0`, parses its
stderr address, polls health, reads OpenAPI and every service's read routes,
loads the **automatically discovered** cached tiny GGUF, checks ordinary completion
and chat SSE termination, unloads, exercises dataset create/read/delete and
validation/not-found errors, terminates the child, and checks Uvicorn's shutdown
completion. Its temporary application home is automatically removed.

Existing regressions cover ZIP traversal/metadata/weight validation and duplicate
imports; RAG config merge, stale/cancelled ingestion, context budget and failures;
model resolution, engine validation, settings and concurrent operation rejection;
stream exceptions and early disconnect/ASGI send failures; Ollama deletion;
shared HF blob preservation; dataset normalization; and training queue cancellation/
restart history. Mocked integrations prove protocol/control behavior, not that a
real external server or GPU combination works.

### Reproduction commands

From the repository root, with dependencies already installed:

```bash
PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests -v
PYTHONPATH=backend/src HF_HUB_OFFLINE=1 TRANSFORMERS_OFFLINE=1 WANDB_MODE=disabled \
  .venv/bin/python backend/tests/live_workflow.py
PYTHONPATH=backend/src .venv/bin/python backend/tests/live_backend_smoke.py
.venv/bin/python -m compileall -q backend/src backend/tests
git diff --check
```

The smoke script requires the named tiny GGUF already cached; it does not download
it. The real ML workflow requires ML extras and cached tiny-gpt2/MiniLM when run
with these offline flags. Running that script without offline flags can download
missing models. Unit tests have no real ML/model-download requirement.

The sandbox blocked loopback socket creation with `PermissionError: Operation not
permitted`; even in-process async tests stalled waiting for worker responses.
The stalled baseline was interrupted, a diagnostic run timed out, and successful
async/live verification was run outside that sandbox. These sandbox attempts
are not counted as backend product failures.

Observed environment: Python 3.12.12, FastAPI 0.141.1, Uvicorn 0.52.4, HTTPX
0.28.1, Pydantic 2.13.5, huggingface-hub 1.30.0, torch 2.14.0, transformers 5.16.1,
PEFT 0.20.0, llama-cpp-python 0.3.35, sentence-transformers 6.0.1. This is evidence
for the installed environment, not all allowed dependency versions.

## Remaining findings and follow-up priorities

These are source-review findings unless explicitly labelled as observed during a
test. They are intentionally not presented as fixed or fully reproduced failures.

### High priority: resource and data lifecycle

1. **Training and inference share resources without a common reservation.**
   `training/jobs.py` serializes training jobs, while `inference/supervisor.py`
   guards only inference operations. A large training job and a loaded model can
   exhaust memory together. Add a shared resource policy and real OOM/recovery
   tests before promising concurrent use.
2. **Dataset/model deletion guards are not atomic with training start.**
   Dataset deletion has no queued/running-job guard. Model deletion examines
   existing jobs but does not hold the job-manager lock through deletion.
   A queued dataset can disappear before use, or training can begin after a model
   check. Reserve concrete dataset/model paths for job lifetime and coordinate
   deletion with that reservation. Existing reference comparisons also do not
   fully canonicalize IDs, repo names, symlinks and paths.
3. **Shutdown has bounded waits, not guaranteed worker completion.**
   Training shutdown joins for two seconds; pulls are daemon threads; desktop
   termination escalates after fifteen seconds. Inference unload rejects a busy
   operation. Test native hangs/forced termination and vLLM descendant cleanup;
   use explicit worker ownership and shutdown states where necessary.
4. **Training does not honor a selected cached revision.**
   `trainer._resolve_base()` converts registry IDs back into repository IDs.
   Inference consults the registry path. Training/adapter reload may therefore
   use a different revision or access the network. Persist and resolve concrete
   base revisions consistently across training and inference.

### Medium priority: correctness and error reporting

5. **The model library is not a capability catalog.** Discovery recognizes weight
   layouts, not whether an architecture can generate text. Embedding models
   remain visible. Auto chooses torch for non-GGUF directories even if unavailable;
   GGUF selection chooses the smallest file and is unsuitable for arbitrary split
   or projector-containing repos. Add capability metadata and explicit file/
   quantization selection; test unsupported architecture responses.
6. **Stale models remain registered.** Failed/offline scans preserve data by design,
   and neither HF nor Ollama refresh removes stale rows. An old HF row can fall
   back to its repo name and trigger fetching on load. Add an explicit unavailable
   state and user-controlled reconciliation, without treating temporary cache or
   server outages as deletion.
7. **Pull jobs are incomplete lifecycle objects.** No persisted history, cancellation,
   deduplication or job-count limit. `_ProgressTqdm` updates `current`/`total` in
   the job dictionary rather than the public `progress` field; real percentage
   can remain zero until completion. It also opens a null-output file without
   explicitly closing it. Network pull progress was not live-tested in this audit.
   Hub metadata failure after a completed download marks the job failed; a new
   regression confirms local discovery recovers the downloaded model on refresh.
8. **Server stream truncation can look like success.** Ollama/vLLM adapters do not
   require upstream terminal markers before exhaustion. The outer response can
   emit `done` after a truncated upstream response. Add truncated-stream tests and
   require explicit terminal status for those protocols.
9. **Error mapping is inconsistent.** Busy load/unload becomes 409, while adapter
   activation's broad handler can produce 500. Hub search/network failures and
   some file/PDF errors can become unstructured 500s. Centralize error categories
   while retaining helpful local diagnostics.
10. **Availability and context are approximate.** Ollama is advertised as available
    without probing its server; vLLM availability is an import check. Remote
    context/token counting uses a 2048-context byte-count fallback rather than
    model metadata/tokenization. Distinguish installed, reachable, compatible,
    and loaded; test budgets against the real engine.
11. **Persistent file operations lack coordinated transactions.** Dataset files and
    DB rows are written separately. Adapter export writes directly to its final
    ZIP path without locking against another export/deletion; an absent weight
    directory can produce a metadata-only archive. Use atomic file publication
    and appropriate operation locks. SQLite connections are generally used as
    transaction contexts, which do not explicitly close connections; tighten
    connection ownership, including RAG `_embedding_dim()`.
12. **Numerical training health needs validation.** The real tiny-model run logged
    one `grad_norm: nan` while completing training and adapter inference. Passing
    these workflows does not establish stable convergence. Validate finite
    metrics, serialize failure states predictably, and assess training quality
    separately from pipeline delivery.

### Scale, portability, and maintenance

13. **RAG is a small local index.** Each query loads matching vectors and sorts all
    scores. Ingestion embeds all chunks together, and input sizes are not bounded
    by the API. Chunk units are words; character-based truncation can cut source
    labels/passages. Model revisions for embeddings are not pinned, and cached
    embedding models are not explicitly unloaded at shutdown. Large corpora,
    concurrent embedder changes, PDF failures and OCR require separate work.
14. **Streaming has no explicit backpressure.** `threaded_stream()` uses an unbounded
    async queue, while nonstreaming requests accumulate the entire response.
    Native worker joins have no deadline. Test slow consumers and hung workers.
15. **Startup assumptions remain.** Free-port selection has a close/rebind race;
    health polling has no overall desktop deadline; Linux parent-death setup is
    best effort. FastAPI startup/shutdown use deprecated `on_event` hooks. Module
    singletons complicate multiple embedded app lifespans. A lifespan-owned
    application factory would make cleanup and testing easier.
16. **Dependency and platform coverage is limited.** `pyproject.toml` uses broad
    minimum versions with no lock. CI covers the base backend and Linux desktop,
    not real ML, vLLM/Ollama, CUDA quantization, Windows or macOS. In particular,
    CPU-only installation without `[ml]` supports GGUF inference; transformer
    CPU inference still needs torch/transformers/PEFT extras.
17. **Trust boundary is local, not multi-user hosting.** API operations accept local
    paths and have no application authentication, quotas or per-user isolation.
    Loopback binding is appropriate for the desktop scope; public serving would
    require a separate design. RAG passages are concatenated into the prompt;
    there is no special untrusted-document instruction boundary.

## Not verified by this audit

- Live Hub search/pull, gated/private repositories, authentication, interruption
  and resume, bandwidth/disk-full behavior, or huge cache performance.
- Live vLLM or Ollama serving, 4/8-bit bitsandbytes training, GPU memory saturation,
  simultaneous training/inference, or every model present in the cache.
- Large-model answer quality, fine-tuning effectiveness, multilingual/token-budget
  correctness, or ANN-scale retrieval performance.
- Forced-kill recovery under every stage, damaged/migrated databases, multi-process
  writers, symlink/path alias races, or full disk/power-loss consistency.
- macOS/Windows execution, packaging, or clean installation of every supported
  dependency version. Blocking ML imports tests the missing-dependency behavior;
  it is not a clean-install test.
- The user's uncommitted desktop/design work, visual/theme changes, or hosted CI.

These limits remain release follow-ups. The delivered cache-discovery fix is
verified independently of them.
