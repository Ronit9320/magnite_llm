# Bramble Studio support triage

Train a small model to route incoming support tickets for a fictional creative
studio. Each response gives the studio’s queue name and a standard next action.
All tickets were written for this demo and use the repository’s MIT license.

## Before and after

In an offline run through Magnite’s backend, the base model matched **0 of 16**
held-out replies and the LoRA adapter matched **15 of 16**. The adapter routed
one workspace removal request to Scope Desk instead of Key Desk (`key-03`).
Training took 113 seconds for 180 steps on an Intel Core i5-12450HX CPU with
the model already downloaded. [Full prompts and raw replies](results.json)

## Record the demo

Start in Playground with this unseen ticket (`asset-03`). Paste the two lines
as one message.

```text
Triage this Bramble Studio support ticket.
Both PDFs are marked approved, but one is for an older product. Which file was actually sent to the printer?
```

**Base model**

> The PDFs are marked approved, but the one for the older Bramble Studio support ticket is marked for an older product.

**With the LoRA adapter**

```text
Queue: Asset Desk
Next: Check the approved handoff.
```

Show the base reply, import the dataset, then cut from the training curve to the
completed run. Activate the adapter and paste the same ticket in a fresh chat.
If you shorten the training footage, label the cut. End with the measured score
and an invitation to try the same dataset in Magnite.

Suggested caption — “Teach a local model your support workflow. We trained a
small model in Magnite on 120 fictional tickets. It went from 0 to 15 exact
matches on 16 unseen tickets. Try the dataset yourself.”

## Routing rules

| Queue | Requests it handles | Next action |
| --- | --- | --- |
| Ledger Desk | Invoices, receipts, charges, credits and refunds | Check the payment record. |
| Asset Desk | Missing or broken copies of approved deliverables | Check the approved handoff. |
| Key Desk | Sign-in, invitations, workspace roles and permissions | Check workspace permissions. |
| Scope Desk | Changes to the work, deliverables or schedule | Review scope and timing. |

Several tickets mention two queues’ subject areas. Route by what the sender wants
done. For example, a paid invoice can be background for a request to add a new
deliverable.

## Run in Magnite

1. In **Library**, download the transformer weights for
   `HuggingFaceTB/SmolLM2-135M-Instruct`. The Python runtime needs Magnite’s ML
   dependencies for LoRA and torch. Use the [CPU setup below](#prepare-the-model-for-the-cpu-recipe)
   for the recorded recipe. [Model card](https://huggingface.co/HuggingFaceTB/SmolLM2-135M-Instruct)
2. In **Playground → Model settings**, select **Local (torch)**, set **Temperature**
   to `0` and **Response tokens** to `64`. Keep **Knowledge** off. Load the model
   and send a `prompt` from [held-out.jsonl](held-out.jsonl) in a fresh chat.
   Record the response. Unload the model before training.
3. In **Training → New run**, select the same model and use **Import dataset…**
   to import [train.jsonl](train.jsonl). It contains 120 tickets, with 30 per
   queue. Keep the 16 tickets in `held-out.jsonl` for evaluation in Playground.
4. Name the adapter `Bramble Studio Triage`, choose **LoRA**, and enter the
   settings below. Start local training.
5. In **Adapters**, activate `Bramble Studio Triage`. Open a fresh chat and send
   the identical prompt with the same generation settings. Verify the adapter
   is active in **Model settings**.

| Setting | Value | Setting | Value |
| --- | --- | --- | --- |
| Epochs | 3 | Learning rate | 0.0005 |
| LoRA rank | 16 | Batch size | 2 |
| Validation holdout | 0% | LoRA alpha | 32 |
| Dropout | 0.05 | Maximum sequence | 80 |
| Warmup ratio | 0.03 | Target modules | `all-linear` |
| Weight precision | None | | |

For the full comparison, use a fresh chat for every ticket, with the same prompt
and settings on both passes. A response passes when the entire reply matches
`expected` after trimming leading and trailing whitespace. This checks the queue,
next action, and two-line format together.

## Prepare the model for the CPU recipe

The comparison used revision `12fd25f77366fa6b3b4b768ec3050bf629380bac` of
SmolLM2-135M-Instruct. Copy its downloaded snapshot into a separate folder with
the weight and tokenizer files present. Hugging Face snapshots may contain
symlinks, so copy their contents or keep their targets accessible.

In the copy, make these configuration edits before running the demo.

| File | Settings |
| --- | --- |
| `config.json` | Set `torch_dtype` to `"float32"` and `pad_token_id` to `0`. |
| `generation_config.json` | Set `pad_token_id` to `0`. |
| `tokenizer_config.json` | Set `pad_token` to `"<\|endoftext\|>"`. |
| `special_tokens_map.json` | Replace the `pad_token` object with the string `"<\|endoftext\|>"`. |

Float32 sped up training on the test CPU. Using a separate padding token lets
the training loss include the token that ends a reply. Keep `eos_token_id` at
`2` and the existing tokenizer `eos_token` value. Paste the copied folder’s path
into Playground’s model selector for the base pass and choose the same folder
with **Model folder…** in Training. Keep the folder available for adapter
activation.
