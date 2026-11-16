# Invoice-email extraction: before/after a LoRA adapter

Teach a small local model to read a messy invoice email and reply with **one JSON
record** that accounting software can use directly. The same task is run on a
base model and on the same model with a LoRA adapter, and both are scored on 300
emails the model never saw in training.

All data is **synthetic**: `generate.py` invents every company, invoice number and
amount (seeded, stdlib only, identical output on every run). No real emails or
customers are involved. Same MIT licence as the rest of the repository.

An illustrative reply (always exactly this shape, keys in this order, no other text):

```text
{"invoice_number":"HF-20417","vendor":"Harlow & Finch","amount":1280.5,"currency":"USD","due_date":"2026-03-03","po_number":null,"payment_terms":"net30"}
```

## Files

| File | What it is |
| --- | --- |
| `train.jsonl` | 3,000 training rows (`instruction` / `input` / `output`) |
| `train-200.jsonl`, `train-1000.jsonl` | The first 200 / 1,000 rows of the same file, for a quick run or a learning curve |
| `held-out.jsonl` | 300 test emails: `id`, `prompt`, `expected`, `tags`, `hard`, `sample` |
| `generate.py` | Regenerates every file above (`python3 generate.py`) |
| `score.py` | Scores a file of replies against `held-out.jsonl` |

## What makes the test fair (and hard)

- The `prompt` already states the full output spec. The base model is told exactly
  what to produce; the question is whether it does so reliably.
- Train and held-out share **no** vendor names and no identical email text. The
  held-out set also uses things the model never saw in training: 3 extra email
  phrasings, an invoice-number format (`INV.773-ZZ`), an amount style
  (`1'280.50`) and two date styles (`Tue 3 March 2026`, `3-Mar-26`).
- 45 of the 300 held-out emails (15%) are **hard**: a second, already-paid invoice
  in the email, a missing due date (answer is `null`), a forwarded thread, or noisy
  signatures with phone and ticket numbers. They are tagged and scored separately.
- 30 of them have `"sample": true`, for the by-hand check below.

## Models

This README is written for **Qwen/Qwen3-0.6B** (about 0.6B parameters). The
reference run uses **HuggingFaceTB/SmolLM2-135M-Instruct**. Both runs follow the
same method, data and scoring so the results can be compared. A few values differ
by model and are called out below (learning rate, batch size, maximum sequence,
response tokens, and the pad-token edit that only SmolLM2 needs).

1. In **Library**, download the transformer weights for `Qwen/Qwen3-0.6B`. The
   Python runtime needs Magnite's ML dependencies (`pip install -e "backend[ml]"`).
2. Pad token: Qwen3's pad token (`<|endoftext|>`) differs from its end-of-reply
   token (`<|im_end|>`), so no tokenizer edit is needed. (SmolLM2 does need one,
   because its pad and end tokens are the same, and the trainer masks pad tokens
   out of the loss. If you ever swap in a model like that, the reply never learns
   to stop.)

## Settings

Write these down **before** you see any result, and do not tune them afterwards to
improve the score. They are a reasoned starting point and have not been
benchmarked on this dataset.

| Generation (both passes) | Value |
| --- | --- |
| Engine | Local (torch) |
| Temperature | `0` |
| Response tokens | `1024` (Qwen3 may write a `<think>` block first; a short limit would cut off the JSON) |
| Knowledge (RAG) | Off |
| Chat | A fresh chat for every email |

| Training | Value | Training | Value |
| --- | --- | --- | --- |
| Method | LoRA | Learning rate | `0.0002` |
| Rank | 16 | Batch size | 4 |
| Alpha | 32 | Epochs | 2 |
| Dropout | 0.05 | Maximum sequence | `320` |
| Target modules | `all-linear` | Warmup ratio | 0.03 |
| Weight precision | None | Validation holdout | 0% |
| Seed | 42 | Dataset | `train.jsonl` (3,000 rows; 1,500 steps) |

`320` covers the longest Qwen3 training row (318 tokens). The trainer pads every row
to this length, so a larger value only slows training down. For SmolLM2 the longest
row is 379 tokens, so the reference run uses `384`, a learning rate of `0.0005`, and
256 response tokens.

## Run it

1. **Smoke test first.** Train on `train-200.jsonl` (same settings otherwise) and
   confirm the run completes and the loss falls. Note the seconds per step shown in
   the training view, multiply by 1,500 to estimate the full run, and tell the
   owner if that is unreasonable on your hardware. If you are on a CPU and the loss
   is `NaN`, or steps are very slow, try a separate copy of the model folder with
   `torch_dtype` set to `float32` in `config.json`. That is what the support-triage
   recipe did for SmolLM2 on a CPU. It has not been tried for Qwen3 here.
2. **Before.** In **Playground**, load the base model with the generation settings
   above. For each held-out email, copy `prompt` exactly, send it in a fresh chat,
   and save the full reply, including any `<think>` block.
3. **Train.** **Training → New run**, same model, **Import dataset…** →
   `train.jsonl`, name the adapter `Invoice Extractor Qwen3`, enter the training
   settings above.
4. **After.** **Adapters** → activate it, confirm it shows as active in **Model
   settings**, then repeat step 2 with the same emails and the same settings.
5. **Check it switches off.** Click **Deactivate** on the adapter and re-send 3 emails. The base
   behaviour should come back. Also send 3 unrelated prompts with the adapter on
   (for example, "What is the capital of France?") and note whether it now forces
   JSON onto everything.

### How many emails to send by hand

The by-hand route is the 30 emails with `"sample": true`. It is enough to see the
difference, not enough for a stable percentage. For all 300, use the headless
runner `run_eval.py` below. Do not paste 300 emails by hand without asking first.

### Headless runner: `run_eval.py`

Stdlib only. It drives a running backend (the same API the app uses), so the model,
adapter and engine are exactly what the app would load. Use a scratch data folder:

```bash
magnite-backend --port 0 --home /tmp/magnite-eval      # prints the port it chose
python3 run_eval.py --url http://127.0.0.1:PORT --model <model folder or Hub id> --out before.jsonl
python3 run_eval.py --url http://127.0.0.1:PORT --adapter <adapter id>            --out after.jsonl
```

- It loads the model (or activates the adapter, which loads its own base model), checks
  `/inference/status` shows what you asked for, then sends each `prompt` verbatim as a
  fresh one-message chat: temperature 0, 256 response tokens (`--max-tokens`), RAG off.
- `--ids inv-001,inv-002` or `--limit 3` runs a few emails; re-running with the same
  `--out` resumes. `<out>.meta.json` records the settings and the status seen.
- The adapter id comes from `GET /adapters`. To train without the app, `POST
  /datasets/ingest` (`{"path": ".../train.jsonl", "format": "jsonl"}`) and `POST
  /training/start` with the settings above.
- Deactivating an adapter (`POST /inference/adapter` with `{"adapter_id": ""}`) unloads
  the engine; load the base model again before re-sending emails.
- Allow about 20 minutes per 300-email pass for SmolLM2 on an Apple M1.

## Score it

Save the replies as `before.jsonl` and `after.jsonl`, one object per line:

```json
{"id": "inv-001", "reply": "<the raw model text, exactly as shown>"}
```

```bash
python3 score.py before.jsonl --json before-scores.json
python3 score.py after.jsonl  --json after-scores.json
```

`score.py` reports, for all / normal / hard emails: valid JSON (strict: the whole
reply is one JSON object; lenient: a JSON object can be cut out of the reply),
accuracy of each of the 7 fields, and **exact record** (strict JSON with all 7
fields right). That last figure is the headline. A `<think>` block is removed
before scoring and counted separately (`had <think>`), so thinking is reported, not
forgiven silently.

## Report back

Please send: the exact model folder or revision, hardware and device used,
training duration and steps, loss at the start and end, both score printouts, the
`before.jsonl` / `after.jsonl` files, and any email where the adapter got a field
wrong. A result that is mediocre or negative is a useful result. Do not adjust
settings to make it look better.
