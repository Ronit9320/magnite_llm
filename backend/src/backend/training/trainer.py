from __future__ import annotations

import os
import threading
import time

from backend.adapters.schemas import AdapterRecord
from backend.adapters.store import AdapterStore
from backend.training.schemas import TrainingConfig, TrainingJob

_ML_HELP = (
    "Training requires the ML extras. Install with: "
    "uv pip install --python .venv/bin/python -e 'backend[ml]'"
)


def _require_ml():
    try:
        import torch  # noqa: F401
        import transformers  # noqa: F401
        from transformers import (  # noqa: F401
            AutoModelForCausalLM,
            AutoTokenizer,
            BitsAndBytesConfig,
            DataCollatorForLanguageModeling,
            Trainer,
            TrainerCallback,
            TrainerControl,
            TrainerState,
            TrainingArguments,
        )
        import peft  # noqa: F401
        from peft import LoraConfig, get_peft_model, prepare_model_for_kbit_training  # noqa: F401
        from datasets import Dataset  # noqa: F401
    except ImportError as exc:
        raise RuntimeError(_ML_HELP) from exc
    return locals()


class _CancelHandler:
    """Default arg/state/control stand-ins for the cancel path."""

    class Control:
        should_training_stop = False

    def __init__(self):
        self.args = type("Args", (), {})()
        self.state = type("State", (), {"global_step": 0, "log_history": []})()
        self.control = self.Control()


def _resolve_base(ref: str) -> str:
    """Map a user-provided model reference to an HF repo id or local path."""
    ref = (ref or "").strip()
    if not ref:
        raise ValueError("base_model is required")
    if os.path.isdir(ref):
        return ref
    if ref.startswith("huggingface:"):
        return ref[len("huggingface:"):].replace("--", "/")
    if ref.startswith("ollama:"):
        raise ValueError("Ollama models are GGUF files and cannot be LoRA fine-tuned locally.")
    if ref.startswith("hf|"):
        return ref[len("hf|"):]
    return ref


def _quantized(cfg: TrainingConfig) -> bool:
    return cfg.quantization in ("4bit", "8bit")


def _build_model_kwargs(cfg: TrainingConfig, torch, BitsAndBytesConfig):
    kwargs = {}
    if cfg.quantization == "4bit":
        kwargs["quantization_config"] = BitsAndBytesConfig(
            load_in_4bit=True,
            bnb_4bit_quant_type="nf4",
            bnb_4bit_use_double_quant=True,
            bnb_4bit_compute_dtype=torch.float16,
        )
    elif cfg.quantization == "8bit":
        kwargs["quantization_config"] = BitsAndBytesConfig(load_in_8bit=True)
    return kwargs


def _target_modules(cfg: TrainingConfig, model=None, torch=None) -> list[str] | str:
    """Resolve the LoRA target modules for the actual base model.

    Explicit lists ("q_proj,v_proj") are respected when the modules exist on
    the model; otherwise we auto-derive the set of attention/MLP linear layers
    so training works across naming conventions (LLaMA q_proj vs GPT-2 c_attn)."""
    tm = (cfg.target_modules or "").strip()
    if not tm or tm == "all-linear":
        return "all-linear"
    requested = [x.strip() for x in tm.split(",") if x.strip()]

    # No model? Trust the caller's explicit list.
    if model is None or torch is None:
        return requested

    try:
        linear_names = [
            name for name, mod in model.named_modules()
            if isinstance(mod, torch.nn.Linear)
        ]
    except Exception:  # noqa: BLE001
        return requested

    skip = ("lm_head", "embed", "classifier", "score", "head", "wte", "wpe")
    candidates = [n for n in linear_names if not any(s in n for s in skip)]
    found = [n for n in candidates
             if any(r.lower() in n.lower() for r in requested)]
    if found:
        return found
    # Nothing matched the explicit names: fall back while keeping the UI
    # unaware — a helpful message is surfaced via the job status.
    return "all-linear"


def _messages_to_text(tokenizer, messages: list[dict]) -> str:
    try:
        return tokenizer.apply_chat_template(
            messages, tokenize=False, add_generation_prompt=False
        )
    except Exception:  # noqa: BLE001 -- fall back to a readable flat format
        return "\n".join(f"{m.get('role', 'user')}: {m.get('content', '')}"
                         for m in messages)


def _tokenize_jsonl(path: str, tokenizer, cfg: TrainingConfig, Dataset):
    import json

    samples = []
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                sample = json.loads(line)
                # Normalize before constructing Arrow columns: mixed text/chat rows
                # otherwise acquire missing/null fields or lose columns entirely.
                text = (_messages_to_text(tokenizer, sample["messages"])
                        if sample.get("messages") else sample.get("text", ""))
                samples.append({"text": text})
            except json.JSONDecodeError as exc:
                raise ValueError("Stored dataset contains invalid JSON") from exc
    raw = Dataset.from_list(samples)

    def _tok(ex):
        text = ex["text"]
        enc = tokenizer(text, truncation=True, max_length=cfg.max_length,
                        padding="max_length")
        enc["labels"] = list(enc["input_ids"])
        return enc

    return raw.map(_tok, remove_columns=raw.column_names, batched=False)


def run_training(job: TrainingJob, cfg: TrainingConfig, dataset_path: str,
                 adapter_dir_root: str, adapter_store: AdapterStore,
                 cancel_event: threading.Event) -> None:
    """Run a LoRA/QLoRA fine-tune in the caller's (background) thread and
    register the resulting adapter in `adapter_store`."""
    ml = _require_ml()
    torch, AutoModelForCausalLM = ml["torch"], ml["AutoModelForCausalLM"]
    AutoTokenizer = ml["AutoTokenizer"]
    BitsAndBytesConfig = ml["BitsAndBytesConfig"]
    DataCollatorForLanguageModeling = ml["DataCollatorForLanguageModeling"]
    Trainer = ml["Trainer"]
    TrainingArguments = ml["TrainingArguments"]

    class _ProgressCallback(ml["TrainerCallback"]):
        """Bridge from the transformers `Trainer` into the shared `TrainingJob`
        model so `GET /training/status/{id}` pollers see live loss/steps."""

        def __init__(self, job: TrainingJob, cancel_event: threading.Event):
            self._job = job
            self._cancel = cancel_event

        def on_train_begin(self, args, state, control, **kwargs):  # noqa: N802
            self._job.status = "running"
            self._job.progress = 5.0
            self._job.started_at = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
            self._job.message = "Training..."

        def on_log(self, args, state, control, **kwargs):  # noqa: N802
            self._job.current_step = state.global_step
            if state.log_history:
                last = state.log_history[-1]
                if "loss" in last:
                    self._job.loss = float(last["loss"])
                    entry = {
                        "step": state.global_step,
                        "loss": self._job.loss,
                        "learning_rate": float(last.get("learning_rate", 0.0)),
                    }
                    if "grad_norm" in last:
                        entry["grad_norm"] = float(last["grad_norm"])
                    self._job.history.append(entry)
                    if len(self._job.history) > 1000:
                        self._job.history = self._job.history[-1000:]
                    if self._job.total_steps > 0:
                        self._job.progress = 5.0 + 85.0 * min(
                            1.0, state.global_step / self._job.total_steps
                        )
                    self._job.message = (
                        f"Training (step {state.global_step}/{self._job.total_steps}): "
                        f"loss={self._job.loss:.4f}"
                    )

        def on_step_end(self, args, state, control, **kwargs):  # noqa: N802
            if self._cancel.is_set():
                control.should_training_stop = True

    work_dir = None
    try:
        base = _resolve_base(cfg.base_model)

        job.message = "Loading base model"
        job.status = "running"
        job.started_at = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        job.progress = 2.0
        if cancel_event.is_set():
            raise _Cancelled()

        tokenizer = AutoTokenizer.from_pretrained(base)
        if tokenizer.pad_token is None:
            tokenizer.pad_token = tokenizer.eos_token

        load_kwargs = _build_model_kwargs(cfg, torch, BitsAndBytesConfig)
        model = AutoModelForCausalLM.from_pretrained(base, **load_kwargs)
        architecture = ""
        try:
            archs = getattr(model.config, "architectures", None) or []
            if archs:
                architecture = archs[0] if isinstance(archs, list) else str(archs)
            else:
                architecture = getattr(model.config, "model_type", "") or ""
        except Exception:  # noqa: BLE001
            architecture = getattr(model.config, "model_type", "") or ""

        job.message = "Preparing dataset"
        if cancel_event.is_set():
            raise _Cancelled()

        from peft import LoraConfig, get_peft_model, prepare_model_for_kbit_training  # noqa: F811
        from datasets import Dataset  # noqa: F811

        if _quantized(cfg):
            model = prepare_model_for_kbit_training(model)

        resolved_targets = _target_modules(cfg, model, torch)
        explicit = _target_modules(cfg)
        if isinstance(resolved_targets, list) and resolved_targets != explicit:
            job.message = (f"Auto-mapped target modules to: "
                           f"{', '.join(resolved_targets)}")
        elif (resolved_targets == "all-linear"
              and isinstance(explicit, list) and explicit):
            job.message = ("Requested target modules not found on this model; "
                           "training all linear layers")

        lora = LoraConfig(
            r=cfg.rank,
            lora_alpha=cfg.alpha,
            target_modules=resolved_targets,
            lora_dropout=0.05,
            bias="none",
            task_type="CAUSAL_LM",
        )
        model = get_peft_model(model, lora)

        train_ds = _tokenize_jsonl(dataset_path, tokenizer, cfg, Dataset)
        steps_per_epoch = max(1, (len(train_ds) + cfg.batch_size - 1) // cfg.batch_size)
        total_steps = cfg.max_steps if cfg.max_steps > 0 else steps_per_epoch * cfg.epochs
        job.total_steps = total_steps
        warmup_steps = int(cfg.warmup_ratio * total_steps)

        args = TrainingArguments(
            output_dir=os.path.join(adapter_dir_root, "_work"),
            per_device_train_batch_size=cfg.batch_size,
            num_train_epochs=cfg.epochs,
            max_steps=cfg.max_steps if cfg.max_steps > 0 else -1,
            learning_rate=cfg.learning_rate,
            warmup_steps=warmup_steps,
            logging_steps=1,
            report_to=[],
            save_strategy="no",
            remove_unused_columns=False,
            fp16=bool(torch.cuda.is_available()),
            gradient_checkpointing=_quantized(cfg),
            dataloader_pin_memory=False,
            disable_tqdm=True,
            seed=cfg.seed,
            use_cpu=False,
        )

        collator = DataCollatorForLanguageModeling(tokenizer=tokenizer, mlm=False)

        progress = _ProgressCallback(job, cancel_event)

        trainer = Trainer(
            model=model,
            args=args,
            train_dataset=train_ds,
            data_collator=collator,
            callbacks=[progress],
        )

        try:
            train_result = trainer.train()
        except SystemExit:
            raise
        except Exception as exc:  # noqa: BLE001
            if cancel_event.is_set():
                raise _Cancelled() from exc
            raise

        if cancel_event.is_set():
            raise _Cancelled()

        job.message = "Saving adapter"
        job.progress = 95.0

        work_dir = os.path.join(adapter_dir_root, cfg.name or f"adapter-{job.id[:6]}")
        os.makedirs(work_dir, exist_ok=True)
        model.save_pretrained(work_dir)
        try:
            tokenizer.save_pretrained(work_dir)
        except Exception:  # noqa: BLE001 -- tokenizer save is best-effort
            pass

        size_mb = 0.0
        for root, _dirs, files in os.walk(work_dir):
            for name in files:
                try:
                    size_mb += os.path.getsize(os.path.join(root, name))
                except OSError:
                    pass
        size_mb = round(size_mb / (1024 * 1024), 2)

        adapter_id = f"adapter:{job.id}"
        rec = AdapterRecord(
            id=adapter_id,
            name=cfg.name or job.name,
            base_model=cfg.base_model,
            architecture=architecture,
            rank=cfg.rank,
            alpha=cfg.alpha,
            target_modules=cfg.target_modules,
            method="qlora" if _quantized(cfg) else "lora",
            quantization=cfg.quantization,
            path=work_dir,
            size_mb=size_mb,
            training_job_id=job.id,
            trained_at=time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            meta={"current_step": train_result.global_step, "total_steps": total_steps},
            status="ready",
        )
        adapter_store.upsert(rec)
        job.adapter_id = adapter_id

        job.status = "done"
        job.progress = 100.0
        job.loss = train_result.training_loss
        job.finished_at = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
        job.message = (f"Done — adapter '{rec.name}' saved ({size_mb} MB)")
    except _Cancelled:
        job.status = "cancelled"
        job.error = None
        job.message = "Cancelled by user"
        job.finished_at = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    except Exception as exc:  # noqa: BLE001
        job.status = "error"
        job.error = str(exc)
        job.message = f"Failed: {exc}"
        job.finished_at = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    finally:
        if work_dir and _is_empty(work_dir):
            try:
                os.rmdir(work_dir)
            except OSError:
                pass


class _Cancelled(Exception):
    """Internal sentinel: training was cancelled by the user."""


def _is_empty(path: str) -> bool:
    try:
        return not os.listdir(path)
    except OSError:
        return True
