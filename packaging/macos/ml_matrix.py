"""Measure what the bundled ML stack actually does on this Mac (macOS packaging, Phase 6).

Run with the BUNDLED interpreter so the shipped code and wheels are what gets measured:

    R=build/packaging/macos/stage/Magnite.app/Contents/Resources/runtime/python/bin/python3
    "$R" -I -B packaging/macos/ml_matrix.py --out docs/release/macos/ML_MATRIX.md \\
         --cache build/packaging/macos/test-cache

Every cell runs in its own `-I -B` subprocess (a crash, hang or abort cannot take the run down; wall
time and peak RSS come from wait4) and goes through the backend's own code paths where the app has
one: LlamaCppEngine with the supervisor's n_gpu_layers, TorchPeftEngine, run_training, rag.embeddings.
bitsandbytes cells use a throwaway APFS clone of the runtime with bitsandbytes added; the shipped
runtime is never modified. Only the section between the ml_matrix markers in --out is rewritten.
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import re
import shutil
import subprocess
import sys
import threading
import time
from pathlib import Path

GGUF_REPO = "Segilmez06/SmolLM2-135M-Instruct-Q4_K_M-GGUF"
TORCH_REPO = "HuggingFaceTB/SmolLM2-135M-Instruct"
EMBED_REPO = "sentence-transformers/all-MiniLM-L6-v2"
# A realistic chat size (28 layers, so the app's n_gpu_layers=20 is a genuine partial offload).
LARGE_GGUF = ("Qwen/Qwen2.5-1.5B-Instruct-GGUF", "qwen2.5-1.5b-instruct-q4_k_m.gguf")
PROMPT = "The three primary colours are"
NEW_TOKENS = 48              # text sample
BENCH_TOKENS = 128           # throughput: one warm-up, then the median of several runs
APP_GPU_LAYERS = 20          # InferenceSupervisor.make_engine on Apple silicon
BEGIN, END = "<!-- ml_matrix:begin -->", "<!-- ml_matrix:end -->"
REPO = Path(__file__).resolve().parents[2]


# ---------------------------------------------------------------- cells (run in the child)

def _result(**kw):
    print("RESULT " + json.dumps(kw, default=str), flush=True)


def _verbose_llama():
    """Make LlamaCppEngine's Llama(...) log to stderr so the offload line can be read back."""
    import llama_cpp
    original = llama_cpp.Llama

    def verbose(*args, **kwargs):
        kwargs["verbose"] = True
        return original(*args, **kwargs)
    llama_cpp.Llama = verbose


def cell_llamacpp(gpu_layers: int, large: bool = False):
    import gc
    _verbose_llama()
    from backend.inference.engines.llamacpp import LlamaCppEngine
    if large:
        from huggingface_hub import hf_hub_download
        ref = hf_hub_download(*LARGE_GGUF)       # the engine accepts a local .gguf path as is
    else:
        ref = GGUF_REPO
    engine = LlamaCppEngine(ref, n_gpu_layers=gpu_layers)
    t = time.perf_counter(); engine.load(); first_load = time.perf_counter() - t
    out = engine._llm.create_completion(PROMPT, max_tokens=NEW_TOKENS, temperature=0.0)
    tokens = out["usage"]["completion_tokens"]
    rates = []
    for _ in range(5):
        t = time.perf_counter()
        bench = engine._llm.create_completion(PROMPT, max_tokens=BENCH_TOKENS, temperature=0.0)
        rates.append(bench["usage"]["completion_tokens"] / (time.perf_counter() - t))
    engine._llm.close(); engine._llm = None; gc.collect()
    t = time.perf_counter(); engine.load(); second_load = time.perf_counter() - t
    text = out["choices"][0]["text"]
    _result(ok=bool(text.strip()) and tokens > 0, first_load_s=round(first_load, 2), second_load_s=round(second_load, 2),
            tokens=tokens, tokens_per_s=round(sorted(rates)[2], 1), tokens_per_s_range=f"{min(rates):.0f}-{max(rates):.0f}",
            text=text.strip()[:160], n_gpu_layers=gpu_layers)


def _greedy(model, tokenizer, device, bench=True):
    import torch
    inputs = tokenizer(PROMPT, return_tensors="pt").to(device)
    with torch.no_grad():
        logits = model(**inputs).logits
        finite = bool(torch.isfinite(logits).all())
        ids = model.generate(**inputs, max_new_tokens=NEW_TOKENS, do_sample=False,
                             pad_token_id=tokenizer.eos_token_id)       # also the warm-up
        rates = []
        for _ in range(3 if bench else 0):
            t = time.perf_counter()
            out = model.generate(**inputs, max_new_tokens=BENCH_TOKENS, min_new_tokens=BENCH_TOKENS,
                                 do_sample=False, pad_token_id=tokenizer.eos_token_id)
            if device == "mps":
                torch.mps.synchronize()
            rates.append((out.shape[1] - inputs["input_ids"].shape[1]) / (time.perf_counter() - t))
    new = ids[0, inputs["input_ids"].shape[1]:]
    return dict(finite_logits=finite, tokens=int(new.shape[0]),
                tokens_per_s=round(sorted(rates)[1], 1) if rates else None,
                text=tokenizer.decode(new, skip_special_tokens=True).strip()[:160])


def cell_torch_cpu():
    import torch
    from transformers import AutoModelForCausalLM, AutoTokenizer
    t = time.perf_counter()
    tokenizer = AutoTokenizer.from_pretrained(TORCH_REPO)
    model = AutoModelForCausalLM.from_pretrained(TORCH_REPO, torch_dtype=torch.float32).to("cpu").eval()
    load = time.perf_counter() - t
    r = _greedy(model, tokenizer, "cpu")
    _result(ok=r["finite_logits"] and r["tokens"] > 0, load_s=round(load, 2), device="cpu", dtype="float32", **r)


def cell_torch_mps():
    import torch
    from backend.inference.engines.torch import TorchPeftEngine
    engine = TorchPeftEngine(TORCH_REPO)          # the app's adapter engine picks mps + float16 itself
    t = time.perf_counter(); engine.load(); load = time.perf_counter() - t
    model = engine._model
    device = str(next(model.parameters()).device.type)
    dtype = str(next(model.parameters()).dtype).replace("torch.", "")
    r = _greedy(model, engine._tokenizer, device)
    _result(ok=r["finite_logits"] and r["tokens"] > 0 and device == "mps", load_s=round(load, 2), device=device,
            dtype=dtype, mps_driver_mb=round(torch.mps.driver_allocated_memory() / 2**20), **r)


def _dataset(path: Path):
    facts = [("What colour is the sky on a clear day?", "Blue."), ("How many legs does a spider have?", "Eight."),
             ("What is the boiling point of water at sea level?", "100 degrees Celsius."),
             ("Which planet is known as the red planet?", "Mars."), ("What is 7 times 6?", "42."),
             ("What language is spoken in Brazil?", "Portuguese."), ("What gas do plants absorb?", "Carbon dioxide."),
             ("How many days are in a leap year?", "366.")]
    with open(path, "w", encoding="utf-8") as f:
        for q, a in facts * 2:
            f.write(json.dumps({"messages": [{"role": "user", "content": q},
                                             {"role": "assistant", "content": a}]}) + "\n")


def cell_train(work: Path, quantization: str, force_cpu: bool):
    import transformers
    if force_cpu:
        # What a CPU fallback in trainer.py would do (it hard-codes use_cpu=False today). Patch the
        # class itself: `from transformers import TrainingArguments` ignores attributes set on the
        # lazy module.
        from transformers.training_args import TrainingArguments
        post_init = TrainingArguments.__post_init__

        def cpu_post_init(self):
            self.use_cpu = True
            post_init(self)
        TrainingArguments.__post_init__ = cpu_post_init
    from backend.adapters.store import AdapterStore
    from backend.inference.engines.torch import TorchPeftEngine
    from backend.training.schemas import TrainingConfig, TrainingJob
    from backend.training.trainer import TrainingCancellation, run_training

    seen = {}
    original_train = transformers.Trainer.train

    def train(self, *a, **kw):
        seen["device"] = str(self.args.device)
        seen["fp16"] = bool(self.args.fp16)
        seen["param_device"] = str(next(self.model.parameters()).device)
        return original_train(self, *a, **kw)
    transformers.Trainer.train = train

    work.mkdir(parents=True, exist_ok=True)
    data = work / "dataset.jsonl"; _dataset(data)
    cfg = TrainingConfig(base_model=TORCH_REPO, dataset_id="ml-matrix", name=f"matrix-{quantization}",
                         quantization=quantization, max_steps=12, batch_size=2, max_length=96,
                         learning_rate=5e-4, rank=8, alpha=16)
    job = TrainingJob(id=f"matrix-{quantization}-{'cpu' if force_cpu else 'auto'}", name=cfg.name, status="queued",
                      base_model=cfg.base_model, dataset_id=cfg.dataset_id)
    store = AdapterStore(str(work / "adapters.db"))
    t = time.perf_counter()
    run_training(job, cfg, str(data), str(work / "adapters"), store, TrainingCancellation())
    train_s = time.perf_counter() - t
    losses = [h["loss"] for h in job.history]
    finite = bool(losses) and all(l == l and abs(l) != float("inf") for l in losses)
    res = dict(status=job.status, error=job.error, train_s=round(train_s, 1), steps=job.current_step,
               first_loss=losses[0] if losses else None, last_loss=losses[-1] if losses else None,
               finite_loss=finite, **seen)
    ok = job.status == "done" and finite
    if ok:
        # Serve the new adapter the way the app does (TorchPeftEngine hot-switch path).
        rec = store.get(job.adapter_id)
        engine = TorchPeftEngine(TORCH_REPO, adapter_path=rec.path)
        engine.load()
        device = str(next(engine._model.parameters()).device.type)
        r = _greedy(engine._model, engine._tokenizer, device, bench=False)
        res.update(adapter_serve_device=device, adapter_serve_finite=r["finite_logits"],
                   adapter_text=r["text"][:80])
        ok = r["finite_logits"] and r["tokens"] > 0
    _result(ok=ok, **res)


def cell_embeddings():
    import numpy as np
    from backend.rag import embeddings
    texts = ["The cat sat on the mat.", "A kitten is resting on a rug.", "Quarterly revenue grew by four percent."]
    t = time.perf_counter()
    vecs = embeddings.embed_texts(texts, EMBED_REPO)
    first = time.perf_counter() - t
    batch = [f"Sentence number {i} about a topic." for i in range(256)]
    t = time.perf_counter(); embeddings.embed_texts(batch, EMBED_REPO); bulk = time.perf_counter() - t
    related, unrelated = float(vecs[0] @ vecs[1]), float(vecs[0] @ vecs[2])
    _result(ok=bool(np.isfinite(vecs).all()) and related > unrelated, device=str(embeddings._embedder.device),
            dim=int(vecs.shape[1]), first_call_s=round(first, 2), sentences_per_s=round(256 / bulk, 1),
            cos_related=round(related, 3), cos_unrelated=round(unrelated, 3))


def cell_bnb_load(quantization: str):
    import torch
    import bitsandbytes
    from transformers import AutoModelForCausalLM, AutoTokenizer, BitsAndBytesConfig
    from backend.training.schemas import TrainingConfig
    from backend.training.trainer import _build_model_kwargs
    cfg = TrainingConfig(base_model=TORCH_REPO, dataset_id="x", quantization=quantization)
    kwargs = _build_model_kwargs(cfg, torch, BitsAndBytesConfig)      # exactly what training passes
    tokenizer = AutoTokenizer.from_pretrained(TORCH_REPO)
    t = time.perf_counter()
    model = AutoModelForCausalLM.from_pretrained(TORCH_REPO, **kwargs).eval()
    load = time.perf_counter() - t
    device = str(next(model.parameters()).device.type)
    quantized = [type(m).__name__ for m in model.modules() if "Linear4bit" in type(m).__name__
                 or "Linear8bitLt" in type(m).__name__]
    r = _greedy(model, tokenizer, device)
    _result(ok=r["finite_logits"] and r["tokens"] > 0 and bool(quantized), bitsandbytes=bitsandbytes.__version__,
            load_s=round(load, 2), device=device, quantized_layers=len(quantized), **r)


CELLS = {
    # id: (row label, interpreter, function, app path?)
    "llamacpp-metal-app": ("llama.cpp, Metal, n_gpu_layers=20 (the app's setting)", "bundled",
                           lambda a: cell_llamacpp(APP_GPU_LAYERS), "yes: default chat engine"),
    "llamacpp-metal-all": ("llama.cpp, Metal, all layers (n_gpu_layers=-1)", "bundled",
                           lambda a: cell_llamacpp(-1), "no: candidate setting"),
    "llamacpp-cpu": ("llama.cpp, CPU only (n_gpu_layers=0)", "bundled", lambda a: cell_llamacpp(0),
                     "no on Apple silicon (used when no GPU is detected)"),
    "llamacpp-1.5b-metal-app": ("llama.cpp 1.5B, Metal, n_gpu_layers=20 (the app's setting)", "bundled",
                                lambda a: cell_llamacpp(APP_GPU_LAYERS, large=True), "yes: default chat engine"),
    "llamacpp-1.5b-metal-all": ("llama.cpp 1.5B, Metal, all layers", "bundled",
                                lambda a: cell_llamacpp(-1, large=True), "no: candidate setting"),
    "llamacpp-1.5b-cpu": ("llama.cpp 1.5B, CPU only", "bundled", lambda a: cell_llamacpp(0, large=True),
                          "no on Apple silicon"),
    "torch-cpu-infer": ("torch inference, CPU, float32", "bundled", lambda a: cell_torch_cpu(), "no: reference"),
    "torch-mps-infer": ("torch inference, MPS, float16 (TorchPeftEngine)", "bundled", lambda a: cell_torch_mps(),
                        "yes: adapter/torch engine"),
    "lora-mps": ("LoRA training, default device (MPS, float32)", "bundled",
                 lambda a: cell_train(a.work / "lora-mps", "none", False), "yes: every training job"),
    "lora-cpu": ("LoRA training, CPU (TrainingArguments use_cpu=True)", "bundled",
                 lambda a: cell_train(a.work / "lora-cpu", "none", True), "no: fallback only"),
    "embeddings": ("RAG embeddings (sentence-transformers)", "bundled", lambda a: cell_embeddings(),
                   "yes: knowledge ingest/query"),
    "qlora-4bit-shipped": ("QLoRA 4-bit in the SHIPPED runtime (what the UI option does today)", "bundled",
                           lambda a: cell_train(a.work / "qlora-4bit-shipped", "4bit", False),
                           "yes: Training > quantization 4bit"),
    "qlora-8bit-shipped": ("QLoRA 8-bit in the SHIPPED runtime (what the UI option does today)", "bundled",
                           lambda a: cell_train(a.work / "qlora-8bit-shipped", "8bit", False),
                           "yes: Training > quantization 8bit"),
    "bnb-4bit-load": ("bitsandbytes 4-bit load + generate", "bnb", lambda a: cell_bnb_load("4bit"),
                      "no: bitsandbytes is not shipped"),
    "bnb-8bit-load": ("bitsandbytes 8-bit load + generate", "bnb", lambda a: cell_bnb_load("8bit"),
                      "no: bitsandbytes is not shipped"),
    "qlora-4bit": ("QLoRA training, 4-bit (bitsandbytes added)", "bnb", lambda a: cell_train(a.work / "qlora-4bit", "4bit", False),
                   "no: only with bitsandbytes added"),
    "qlora-8bit": ("QLoRA training, 8-bit (bitsandbytes added)", "bnb", lambda a: cell_train(a.work / "qlora-8bit", "8bit", False),
                   "no: only with bitsandbytes added"),
}


def prefetch(_args):
    from huggingface_hub import snapshot_download
    from backend.inference.engines.llamacpp import LlamaCppEngine
    # Only what transformers / sentence-transformers load (the repos also carry ONNX/GGUF exports, GBs).
    for repo in (TORCH_REPO, EMBED_REPO):
        snapshot_download(repo, allow_patterns=["*.json", "*.safetensors", "*.txt", "*.model", "1_Pooling/*"])
    LlamaCppEngine(GGUF_REPO)._resolve_gguf_path()
    from huggingface_hub import hf_hub_download
    hf_hub_download(*LARGE_GGUF)
    _result(ok=True)


# ---------------------------------------------------------------- orchestration (parent)

def run_child(python: str, cell: str, args, log: Path, timeout: int) -> dict:
    env = {"HOME": os.environ["HOME"], "PATH": "/usr/bin:/bin", "HF_HOME": str(args.cache),
           "HF_HUB_DISABLE_TELEMETRY": "1", "TOKENIZERS_PARALLELISM": "false", "TMPDIR": os.environ.get("TMPDIR", "/tmp")}
    cmd = [python, "-I", "-B", str(Path(__file__).resolve()), "--cell", cell,
           "--cache", str(args.cache), "--work", str(args.work)]
    t = time.perf_counter()
    with open(log, "w") as err:
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=err, env=env, text=True)
        timer = threading.Timer(timeout, proc.kill); timer.start()
        out = proc.stdout.read()
        _, status, usage = os.wait4(proc.pid, 0)
        timer.cancel()
    wall = time.perf_counter() - t
    proc.returncode = os.waitstatus_to_exitcode(status)
    lines = [l for l in out.splitlines() if l.startswith("RESULT ")]
    res = json.loads(lines[-1][7:]) if lines else {"ok": False}
    res.update(wall_s=round(wall, 1), peak_rss_mb=round(usage.ru_maxrss / 2**20), exit=proc.returncode)
    if not lines:
        tail = [l for l in log.read_text(errors="replace").splitlines() if l.strip()][-6:]
        res["error"] = (f"timed out after {timeout}s; " if proc.returncode == -9 else f"exit {proc.returncode}; ") \
            + " | ".join(tail)
    if cell.startswith("llamacpp"):
        text = log.read_text(errors="replace")
        m = re.findall(r"offloaded (\d+)/(\d+) layers to GPU", text)
        res["offloaded"] = f"{m[0][0]}/{m[0][1]}" if m else "none reported"
        res["metal_device"] = bool(re.search(r"ggml_metal|MTLGPUFamily|using device Metal", text))
    return res


def prepare_bnb_runtime(args, bundled: str) -> tuple:
    """Clone the bundled runtime and add bitsandbytes. Returns (python or None, note)."""
    dest = args.work / "bnb-runtime"
    python = dest / "bin/python3"
    if not python.exists():
        shutil.rmtree(dest, ignore_errors=True)
        dest.parent.mkdir(parents=True, exist_ok=True)
        subprocess.run(["cp", "-c", "-R", str(Path(bundled).resolve().parents[1]), str(dest)], check=True)
    uv = shutil.which("uv")
    if not uv:
        return None, "uv not found; cannot add bitsandbytes to the throwaway runtime"
    env = dict(os.environ, UV_CACHE_DIR=str(REPO / "build/packaging/uv-cache"))
    p = subprocess.run([uv, "pip", "install", "--python", str(python), "--break-system-packages",
                        "-r", str(REPO / "packaging/macos/requirements-macos-quantization.txt")],
                       capture_output=True, text=True, env=env)
    if p.returncode != 0:
        return None, "bitsandbytes install failed: " + p.stderr.strip().splitlines()[-1]
    return str(python), (p.stderr.strip().splitlines() or ["installed"])[-1]


def fmt(v):
    return "–" if v is None else str(v)


def render(results: dict, env_info: dict) -> str:
    lines = [BEGIN, "", f"_Generated by `packaging/macos/ml_matrix.py` on {env_info['date']}; re-running it rewrites only this section._",
             "", "| Environment | |", "|---|---|"]
    lines += [f"| {k} | {v} |" for k, v in env_info.items() if k != "date"]
    lines += ["", "| Cell | Used by the app | Verdict | Wall | Peak RSS | Measured | Error |", "|---|---|---|---|---|---|---|"]
    for cell, (label, _py, _fn, app) in CELLS.items():
        r = results.get(cell, {})
        verdict = "**PASS**" if r.get("ok") else "**FAIL**"
        keys = [k for k in ("device", "dtype", "offloaded", "first_load_s", "second_load_s", "load_s", "tokens_per_s",
                            "tokens_per_s_range", "param_device",
                            "train_s", "first_loss", "last_loss", "fp16", "adapter_serve_device", "sentences_per_s",
                            "cos_related", "cos_unrelated", "quantized_layers", "bitsandbytes", "mps_driver_mb")
                if r.get(k) is not None]
        measured = ", ".join(f"{k}={r[k]}" for k in keys)
        error = (r.get("error") or "").replace("|", "/")[:300]
        lines.append(f"| `{cell}` {label} | {app} | {verdict} | {fmt(r.get('wall_s'))} s | {fmt(r.get('peak_rss_mb'))} MB "
                     f"| {measured} | {error} |")
    lines += ["", "Generated text (greedy, prompt `" + PROMPT + "`):", ""]
    for cell, r in results.items():
        for key in ("text", "adapter_text"):
            if r.get(key):
                lines.append(f"- `{cell}` {key}: {json.dumps(r[key])}")
    lines += ["", END]
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", type=Path)
    ap.add_argument("--cache", type=Path, required=True, help="dedicated HF_HOME for the test models")
    ap.add_argument("--work", type=Path, help="scratch dir (default: <cache>/../ml-matrix)")
    ap.add_argument("--cell", help=argparse.SUPPRESS)
    ap.add_argument("--only", help="comma-separated cell ids to run")
    ap.add_argument("--timeout", type=int, default=900)
    args = ap.parse_args()
    args.cache = args.cache.resolve()
    args.work = (args.work or args.cache.parent / "ml-matrix").resolve()

    if args.cell:
        (prefetch if args.cell == "prefetch" else CELLS[args.cell][2])(args)
        return

    if not args.out:
        ap.error("--out is required")
    args.work.mkdir(parents=True, exist_ok=True)
    logs = args.work / "logs"; logs.mkdir(exist_ok=True)
    bundled = sys.executable
    import torch, transformers, peft, llama_cpp   # noqa: E401 -- versions for the report
    build = json.loads((Path(bundled).resolve().parents[3] / "build-info/build.json").read_text()) \
        if (Path(bundled).resolve().parents[3] / "build-info/build.json").exists() else {}
    mem = int(subprocess.run(["sysctl", "-n", "hw.memsize"], capture_output=True, text=True).stdout or 0)
    env_info = {"date": time.strftime("%Y-%m-%d %H:%M %Z"),
                "Machine": subprocess.run(["sysctl", "-n", "machdep.cpu.brand_string"], capture_output=True, text=True).stdout.strip()
                + f", {mem / 2**30:.0f} GiB RAM",
                "macOS": platform.mac_ver()[0],
                "Interpreter": f"bundled Python {platform.python_version()} ({'runtime of commit ' + build.get('commit', '?')[:7] if build else bundled})",
                "Versions": f"torch {torch.__version__}, transformers {transformers.__version__}, peft {peft.__version__}, "
                            f"llama-cpp-python {llama_cpp.__version__}",
                "Models": f"`{GGUF_REPO}` (135M GGUF Q4_K_M), `{LARGE_GGUF[0]}/{LARGE_GGUF[1]}` (1.5B), "
                          f"`{TORCH_REPO}`, `{EMBED_REPO}`"}

    print("prefetching models into", args.cache, flush=True)
    pre = run_child(bundled, "prefetch", args, logs / "prefetch.log", args.timeout)
    if not pre.get("ok"):
        sys.exit(f"prefetch failed: {pre.get('error')}")

    selected = [c for c in CELLS if not args.only or c in args.only.split(",")]
    results_path = args.work / "results.json"
    results = json.loads(results_path.read_text()) if results_path.exists() else {}
    bnb_python, bnb_note = (None, "")
    if any(CELLS[c][1] == "bnb" for c in selected):
        bnb_python, bnb_note = prepare_bnb_runtime(args, bundled)
        env_info["bitsandbytes runtime"] = f"throwaway clone + `requirements-macos-quantization.txt`: {bnb_note}"
    for cell in selected:
        python = bundled if CELLS[cell][1] == "bundled" else bnb_python
        print(f"[{cell}] ...", end=" ", flush=True)
        if python is None:
            results[cell] = {"ok": False, "error": bnb_note}
        else:
            results[cell] = run_child(python, cell, args, logs / f"{cell}.log", args.timeout)
        r = results[cell]
        print("PASS" if r.get("ok") else "FAIL", f"{r.get('wall_s')} s", (r.get("error") or "")[:200], flush=True)
        results_path.write_text(json.dumps(results, indent=1, default=str))

    section = render(results, env_info)
    doc = args.out.read_text() if args.out.exists() else "# ML matrix — macOS arm64 testing build\n\n"
    if BEGIN in doc and END in doc:
        doc = doc[:doc.index(BEGIN)] + section + doc[doc.index(END) + len(END):]
    else:
        doc = doc.rstrip() + "\n\n" + section + "\n"
    args.out.write_text(doc)
    print(f"wrote {args.out}; raw results {results_path}; logs {logs}")


if __name__ == "__main__":
    main()
