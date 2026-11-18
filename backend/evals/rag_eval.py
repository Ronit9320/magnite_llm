#!/usr/bin/env python3
"""Compare embedding models on your own documents. Not shipped in the app.

Questions file (JSONL), one object per line:
    {"question": "What is the notice period?", "doc": "contract.pdf",
     "answer_contains": ["30 days"]}

A retrieved chunk is a hit when it comes from `doc` and contains every phrase in
`answer_contains` (case-insensitive, whitespace-normalized). That ground truth does not
depend on how the text was chunked, so models with different chunk sizes compare fairly.

Run every default candidate (each in its own process, so peak RAM is clean):
    python backend/evals/rag_eval.py --all --docs ~/my_docs --questions q.jsonl --out results.md
Run one:
    python backend/evals/rag_eval.py --candidate Qwen/Qwen3-Embedding-0.6B --dtype float16 \
        --docs ~/my_docs --questions q.jsonl
Compare vector search with hybrid (FTS5 keyword + vector, fused with RRF) from one embedding pass:
    python backend/evals/rag_eval.py --candidate sentence-transformers/all-MiniLM-L6-v2 --search both \
        --docs ~/my_docs --questions q.jsonl
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sqlite3
import subprocess
import sys
import tempfile
import time
from dataclasses import asdict, dataclass, field
from typing import Callable, Optional

import numpy as np

_HERE = os.path.dirname(os.path.abspath(__file__))
for _src in (os.path.join(_HERE, "..", "src"), os.path.join(_HERE, "src")):   # repo layout, then standalone kit
    if os.path.isdir(os.path.join(_src, "backend")) and _src not in sys.path:
        sys.path.insert(0, _src)

KS = (1, 4, 10)
TOKENS_PER_WORD = 1.7          # measured 1.3-1.65 on real text; conservative
TARGET_CHUNK_TOKENS = 512
BATCH_SIZE = 4                 # small batches keep activations (and GPU cache growth) bounded
RELEASE_EVERY = 8              # batches between accelerator cache releases
EXIT_MEMORY_GUARD = 75         # exit code when the memory guard stops a run
MAX_SEQ_CAP = 1024             # bounds memory for 32k-context models; chunks are far shorter

QWEN_QUERY_PROMPT = ("Instruct: Given a web search query, retrieve relevant passages "
                     "that answer the query\nQuery:")          # exactly the model card's format

# Rough memory each candidate needs while running (MB), for the preflight check.
REQUIRED_MB = {"Qwen/Qwen3-Embedding-0.6B": 1800, "BAAI/bge-m3": 2000,
               "sentence-transformers/all-MiniLM-L6-v2": 400,
               "Qwen/Qwen3-Embedding-4B": 9000}
DEFAULT_REQUIRED_MB = 1500


def prompt_for(model_id: str) -> str:
    """Query instruction prefix: the Qwen3-Embedding family expects one; the others use none."""
    return QWEN_QUERY_PROMPT if "Qwen3-Embedding" in model_id else ""


def candidates(include_large: bool = False) -> list:
    """(model id, dtype, query prompt, document prompt). dtype 'auto' lets each machine pick its own."""
    ids = ["Qwen/Qwen3-Embedding-0.6B", "BAAI/bge-m3", "sentence-transformers/all-MiniLM-L6-v2"]
    if include_large:
        ids.append("Qwen/Qwen3-Embedding-4B")
    # The 90 MB MiniLM stays in float32 (half precision only adds noise); the big models use auto.
    return [(m, "float32" if "MiniLM" in m else "auto", prompt_for(m), "") for m in ids]


def auto_dtype(torch_module) -> str:
    """Half precision wherever the hardware supports it (Qwen is trained in bfloat16)."""
    if torch_module is None:
        return "float32"
    try:
        if torch_module.cuda.is_available():
            return "bfloat16" if torch_module.cuda.is_bf16_supported() else "float16"
        if torch_module.backends.mps.is_available():
            return "bfloat16"
    except (AttributeError, RuntimeError):
        pass
    return "float32"


def resolve_dtype(dtype: str, torch_module) -> str:
    return auto_dtype(torch_module) if dtype == "auto" else dtype


def system_summary() -> str:
    import platform
    from backend.rag import memguard
    parts = [f"{platform.system()} {platform.release()} {platform.machine()}",
             f"Python {platform.python_version()}",
             f"{memguard.total_memory_mb() / 1024:.0f} GB RAM"]
    try:
        import torch
        parts.append(f"torch {torch.__version__}")
        if torch.cuda.is_available():
            parts.append(f"CUDA {torch.cuda.get_device_name(0)}")
        elif torch.backends.mps.is_available():
            parts.append("Apple GPU (MPS)")
        else:
            parts.append("CPU only")
    except ImportError:
        parts.append("torch not installed")
    return ", ".join(parts)


def report_with_header(rows: list, machine: str) -> str:
    return f"Machine: {machine}\n\n{format_report(rows)}"


@dataclass(frozen=True)
class Question:
    question: str
    doc: str
    answer_contains: list


@dataclass
class CandidateResult:
    model: str
    dtype: str
    dim: int
    chunk_words: int
    chunks: int
    recall: dict
    mrr: float
    ingest_chunks_per_s: float
    peak_rss_mb: float
    load_seconds: float = 0.0
    query_ms: float = 0.0
    device: str = ""
    first_hit_ranks: list = field(default_factory=list)   # one per question, 0 = miss in top 10
    notes: list = field(default_factory=list)
    search: str = "vector"                                # vector | hybrid


# ---- pure helpers (unit-tested, no ML imports) ---------------------------------


def _norm(text: str) -> str:
    return re.sub(r"\s+", " ", text).strip().lower()


def is_hit(chunk_text: str, answer_contains: list) -> bool:
    if not answer_contains:
        return False
    haystack = _norm(chunk_text)
    return all(_norm(n) in haystack for n in answer_contains)


def recall_at_k(ranked_hits: list, k: int) -> float:
    if not ranked_hits:
        return 0.0
    return sum(1 for row in ranked_hits if any(row[:k])) / len(ranked_hits)


def mrr(ranked_hits: list) -> float:
    if not ranked_hits:
        return 0.0
    total = 0.0
    for row in ranked_hits:
        for rank, hit in enumerate(row, 1):
            if hit:
                total += 1.0 / rank
                break
    return total / len(ranked_hits)


def first_hit_ranks(ranked_hits: list) -> list:
    """1-based rank of the first hit per question; 0 when nothing in the list is a hit."""
    return [next((i for i, hit in enumerate(row, 1) if hit), 0) for row in ranked_hits]


def recall_by_doc(questions: list, ranks: list, k: int) -> dict:
    """recall@k grouped by each question's document."""
    groups: dict = {}
    for q, rank in zip(questions, ranks):
        groups.setdefault(q.doc, []).append(1 <= rank <= k)
    return {doc: sum(flags) / len(flags) for doc, flags in groups.items()}


def load_questions(path: str) -> list:
    questions = []
    with open(path, "r", encoding="utf-8") as f:
        for number, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                row = json.loads(line)
            except json.JSONDecodeError as exc:
                raise ValueError(f"{path} line {number}: invalid JSON ({exc})") from exc
            needles = row.get("answer_contains") if isinstance(row, dict) else None
            if (not isinstance(row, dict) or not row.get("question") or not row.get("doc")
                    or not isinstance(needles, list) or not needles):
                raise ValueError(
                    f"{path} line {number}: need 'question', 'doc' and a non-empty 'answer_contains' list")
            questions.append(Question(str(row["question"]), str(row["doc"]), [str(n) for n in needles]))
    return questions


def default_chunk_words(max_seq_tokens: int) -> int:
    """Words per chunk: about 512 tokens, but never more than the model can embed."""
    budget = min(max_seq_tokens, TARGET_CHUNK_TOKENS) - 2
    return max(32, int(budget / TOKENS_PER_WORD))


def rank_hits(query_vec: np.ndarray, vecs: np.ndarray, chunks: list, needles: list, k: int,
              chunk_docs: Optional[list] = None, doc: Optional[str] = None) -> list:
    """Hit flags for the top-k chunks by cosine score (vectors are L2-normalized)."""
    scores = vecs @ query_vec.reshape(-1)
    order = np.argsort(-scores, kind="stable")[:k]
    hits = []
    for idx in order:
        from_doc = chunk_docs is None or doc is None or chunk_docs[idx] == doc
        hits.append(bool(from_doc and is_hit(chunks[idx], needles)))
    hits.extend([False] * (k - len(hits)))
    return hits


def build_fts(chunks: list) -> sqlite3.Connection:
    """In-memory FTS5 table over the chunks (rowid = position), tokenized like the store's keyword index."""
    conn = sqlite3.connect(":memory:")
    conn.execute("CREATE VIRTUAL TABLE chunks_fts USING fts5(text, tokenize='unicode61 remove_diacritics 2')")
    conn.executemany("INSERT INTO chunks_fts (rowid, text) VALUES (?, ?)", list(enumerate(chunks)))
    return conn


def rank_hits_hybrid(query_text: str, query_vec: np.ndarray, vecs: np.ndarray, chunks: list, needles: list,
                     k: int, chunk_docs: Optional[list] = None, doc: Optional[str] = None,
                     fts_conn: Optional[sqlite3.Connection] = None) -> list:
    """Hit flags for the top-k chunks by Reciprocal Rank Fusion of vector and FTS5 keyword rankings.

    Mirrors RagStore.search: each leg contributes max(2k, k+16) candidates, the fused order breaks ties
    by higher cosine and then lower chunk position. Without searchable text it equals ``rank_hits``.
    """
    from backend.rag import keyword

    scores = vecs @ query_vec.reshape(-1)
    n = max(2 * k, k + 16)
    vector_ranked = [int(i) for i in np.argsort(-scores, kind="stable")[:n]]
    expr = keyword.match_expression(query_text)
    keyword_ranked: list = []
    if expr is not None and fts_conn is not None:
        keyword_ranked = [int(r[0]) for r in fts_conn.execute(
            "SELECT rowid FROM chunks_fts WHERE chunks_fts MATCH ? ORDER BY rank LIMIT ?", (expr, n))]
    fused = keyword.rrf_fuse([vector_ranked, keyword_ranked])
    order = sorted(fused, key=lambda i: (-fused[i], -float(scores[i]), i))[:k]
    hits = []
    for idx in order:
        from_doc = chunk_docs is None or doc is None or chunk_docs[idx] == doc
        hits.append(bool(from_doc and is_hit(chunks[idx], needles)))
    hits.extend([False] * (k - len(hits)))
    return hits


def evaluate(docs: dict, questions: list, embed_docs: Callable, embed_query: Callable,
             chunk_words: int, overlap: Optional[int] = None, ks=KS, search: str = "vector") -> dict:
    """Chunk every document, embed once, then rank each question against the whole library.

    ``search`` is vector, hybrid or both; every requested mode is ranked from the same embeddings
    (``ranked_by_mode``). ``ranked_hits`` is the first requested mode's ranking.
    """
    from backend.rag.chunking import chunk_text

    modes = ["vector", "hybrid"] if search == "both" else [search]
    if any(m not in ("vector", "hybrid") for m in modes):
        raise ValueError(f"unknown search mode {search!r}")

    overlap = chunk_words // 8 if overlap is None else overlap
    chunks, chunk_docs = [], []
    for name, text in docs.items():
        for piece in chunk_text(text, chunk_words, overlap):
            chunks.append(piece)
            chunk_docs.append(name)
    if not chunks:
        raise ValueError("No chunks produced; check the documents")

    started = time.perf_counter()
    vecs = np.asarray(embed_docs(chunks), dtype=np.float32)
    ingest_seconds = time.perf_counter() - started

    kmax = max(ks)
    fts_conn = build_fts(chunks) if "hybrid" in modes else None
    ranked_by_mode = {m: [] for m in modes}
    query_seconds = 0.0
    for q in questions:
        t0 = time.perf_counter()
        qv = np.asarray(embed_query(q.question), dtype=np.float32)
        query_seconds += time.perf_counter() - t0
        for mode in modes:
            if mode == "vector":
                row = rank_hits(qv, vecs, chunks, q.answer_contains, kmax, chunk_docs, q.doc)
            else:
                row = rank_hits_hybrid(q.question, qv, vecs, chunks, q.answer_contains, kmax, chunk_docs,
                                       q.doc, fts_conn)
            ranked_by_mode[mode].append(row)
    if fts_conn is not None:
        fts_conn.close()
    return {
        "chunks": len(chunks),
        "dim": int(vecs.shape[1]),
        "ranked_hits": ranked_by_mode[modes[0]],
        "ranked_by_mode": ranked_by_mode,
        "ingest_seconds": ingest_seconds,
        "ingest_chunks_per_s": len(chunks) / ingest_seconds if ingest_seconds > 0 else 0.0,
        "query_ms": 1000.0 * query_seconds / max(1, len(questions)),
    }


def format_report(rows: list) -> str:
    head = ("| model | search | dtype | dim | chunk words | chunks | recall@1 | recall@4 | recall@10 | MRR "
            "| ingest chunks/s | load s | query ms | peak footprint MB |\n"
            "| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |")
    lines = [head]
    for r in rows:
        lines.append(
            f"| {r.model} | {r.search} | {r.dtype} | {r.dim} | {r.chunk_words} | {r.chunks} "
            f"| {r.recall[1]:.2f} | {r.recall[4]:.2f} | {r.recall[10]:.2f} | {r.mrr:.2f} "
            f"| {r.ingest_chunks_per_s:.1f} | {r.load_seconds:.1f} | {r.query_ms:.0f} "
            f"| {r.peak_rss_mb:.0f} |")
    return "\n".join(lines)


# ---- model-backed part (only runs in the candidate subprocess) -----------------


def load_documents(directory: str) -> tuple:
    from backend.rag.chunking import detect_kind, extract_text

    docs, notes = {}, []
    for name in sorted(os.listdir(directory)):
        path = os.path.join(directory, name)
        if not os.path.isfile(path) or name.startswith("."):
            continue
        kind = detect_kind(name)
        if os.path.splitext(name)[1].lower().lstrip(".") not in ("txt", "md", "html", "pdf"):
            continue
        try:
            text = extract_text(path, kind)
        except ValueError as exc:
            notes.append(f"skipped {name}: {exc}")
            continue
        docs[name] = text
        notes.append(f"{name}: {len(text):,} chars, {len(text.split()):,} words")
    return docs, notes


def run_candidate(model_id: str, dtype: str, query_prompt: str, document_prompt: str,
                  docs: dict, questions: list, chunk_words: Optional[int],
                  device: Optional[str] = None, search: str = "vector") -> list:
    """One CandidateResult per requested search mode, all from a single model load and embedding pass."""
    from backend.rag import memguard

    memguard.apply_mps_limits()            # must happen before torch is imported
    memguard.preflight(REQUIRED_MB.get(model_id, DEFAULT_REQUIRED_MB))
    guard = memguard.MemoryGuard(memguard.limits_from_env())

    def stop(exc):
        print(f"\nMEMORY GUARD: {exc}", file=sys.stderr, flush=True)
        os._exit(EXIT_MEMORY_GUARD)        # hard stop: the loop may be stuck inside one GPU call

    with guard.watch(stop):
        return _run_guarded(model_id, dtype, query_prompt, document_prompt, docs, questions,
                            chunk_words, device, guard, memguard, search)


def _run_guarded(model_id, dtype, query_prompt, document_prompt, docs, questions, chunk_words,
                 device, guard, memguard, search="vector") -> list:
    from sentence_transformers import SentenceTransformer

    try:
        import torch
    except ImportError:
        torch = None
    dtype = resolve_dtype(dtype, torch)
    started = time.perf_counter()
    try:
        model = SentenceTransformer(model_id, device=device, model_kwargs={"dtype": dtype})
    except TypeError:
        model = SentenceTransformer(model_id, device=device, model_kwargs={"torch_dtype": dtype})
    model.max_seq_length = min(model.max_seq_length or MAX_SEQ_CAP, MAX_SEQ_CAP)
    load_seconds = time.perf_counter() - started
    guard.check()

    def encode(texts):
        parts = []
        for n, start in enumerate(range(0, len(texts), BATCH_SIZE), 1):
            parts.append(model.encode(texts[start:start + BATCH_SIZE], batch_size=BATCH_SIZE,
                                      normalize_embeddings=True, convert_to_numpy=True,
                                      show_progress_bar=False))
            guard.check()
            if n % RELEASE_EVERY == 0:
                memguard.release_accelerator_memory()
        memguard.release_accelerator_memory()
        return np.concatenate(parts, axis=0)

    def embed_docs(texts):
        return encode([document_prompt + t for t in texts])

    def embed_query(text):
        return encode([query_prompt + text])[0]

    words = chunk_words or default_chunk_words(model.max_seq_length)
    result = evaluate(docs, questions, embed_docs, embed_query, words, search=search)
    return [CandidateResult(
        model=model_id, dtype=dtype, dim=result["dim"], chunk_words=words, chunks=result["chunks"],
        recall={k: recall_at_k(ranked, k) for k in KS}, mrr=mrr(ranked),
        ingest_chunks_per_s=result["ingest_chunks_per_s"], peak_rss_mb=guard.peak_footprint_mb,
        load_seconds=load_seconds, query_ms=result["query_ms"], device=str(model.device),
        first_hit_ranks=first_hit_ranks(ranked), search=mode,
    ) for mode, ranked in result["ranked_by_mode"].items()]


def _run_all(args) -> list:
    rows = []
    for model_id, dtype, _q, _d in candidates(args.include_large):
        fd, out = tempfile.mkstemp(suffix=".json")
        os.close(fd)
        cmd = [sys.executable, os.path.abspath(__file__), "--candidate", model_id, "--dtype", dtype,
               "--docs", args.docs, "--questions", args.questions, "--json-out", out]
        if args.chunk_words:
            cmd += ["--chunk-words", str(args.chunk_words)]
        if args.device:
            cmd += ["--device", args.device]
        if args.search != "vector":
            cmd += ["--search", args.search]
        print(f"== {model_id} ({dtype})", flush=True)
        proc = subprocess.run(cmd)
        if proc.returncode == 0:
            with open(out, "r", encoding="utf-8") as f:
                data = json.load(f)
            for item in data:
                item["recall"] = {int(k): v for k, v in item["recall"].items()}
                rows.append(CandidateResult(**item))
        else:
            why = " (stopped by the memory guard)" if proc.returncode == EXIT_MEMORY_GUARD else ""
            print(f"   failed (exit {proc.returncode}){why}; continuing", file=sys.stderr)
        os.remove(out)
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--docs", required=True, help="directory of .txt/.md/.html/.pdf files")
    ap.add_argument("--questions", required=True, help="JSONL question file")
    ap.add_argument("--all", action="store_true", help="run every default candidate")
    ap.add_argument("--candidate", help="one Hugging Face model id")
    ap.add_argument("--dtype", default="auto", choices=["auto", "float16", "bfloat16", "float32"],
                    help="auto = bfloat16 on CUDA/Apple GPU where supported, else float32")
    ap.add_argument("--include-large", action="store_true",
                    help="with --all, also run Qwen3-Embedding-4B (needs ~9 GB free; set MAGNITE_RAG_MAX_MB=12000)")
    ap.add_argument("--query-prompt", default=None)
    ap.add_argument("--document-prompt", default="")
    ap.add_argument("--chunk-words", type=int, default=0, help="override words per chunk")
    ap.add_argument("--search", default="vector", choices=["vector", "hybrid", "both"],
                    help="vector (default), hybrid (FTS5 keyword + vector, RRF), or both from one embedding pass")
    ap.add_argument("--device", default=None, help="cpu, mps or cuda (default: library choice)")
    ap.add_argument("--out", help="write the markdown report here")
    ap.add_argument("--json-out", help=argparse.SUPPRESS)
    args = ap.parse_args()

    if args.all == bool(args.candidate):
        ap.error("pass exactly one of --all or --candidate")

    if args.all:
        rows = _run_all(args)
        machine = system_summary()
        report = report_with_header(rows, machine) if rows else "No candidate finished."
        print("\n" + report)
        if args.out:
            with open(args.out, "w", encoding="utf-8") as f:
                f.write(report + "\n")
            with open(args.out + ".json", "w", encoding="utf-8") as f:
                json.dump({"machine": machine, "chunk_words": args.chunk_words or "per-model",
                           "results": [asdict(r) for r in rows]}, f, indent=1)
        return 0 if rows else 1

    docs, notes = load_documents(args.docs)
    questions = load_questions(args.questions)
    missing = sorted({q.doc for q in questions} - set(docs))
    if missing:
        print(f"Questions reference documents that were not loaded: {missing}", file=sys.stderr)
        return 2
    for note in notes:
        print("  " + note)
    prompt = args.query_prompt
    if prompt is None:
        prompt = prompt_for(args.candidate)
    from backend.rag.memguard import MemoryGuardTripped
    try:
        results = run_candidate(args.candidate, args.dtype, prompt, args.document_prompt,
                                docs, questions, args.chunk_words or None, args.device, args.search)
    except MemoryGuardTripped as exc:
        print(f"MEMORY GUARD: {exc}", file=sys.stderr)
        return EXIT_MEMORY_GUARD
    for result in results:
        result.notes = notes
    if args.json_out:
        with open(args.json_out, "w", encoding="utf-8") as f:
            json.dump([asdict(r) for r in results], f)
    else:
        report = format_report(results)
        print(report)
        if args.out:
            with open(args.out, "w", encoding="utf-8") as f:
                f.write(report_with_header(results, system_summary()) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
