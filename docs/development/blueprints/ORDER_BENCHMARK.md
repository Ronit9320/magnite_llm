# Order Blueprint benchmark

The comparison used all 80 held-out messages from the historical compositional
order experiment. Every variant received the same messages in the same order and
used the SmolLM2-135M-Instruct checkpoint at revision
`12fd25f77366fa6b3b4b768ec3050bf629380bac`. Generation was greedy with the
original per-stage token budgets. The case file's SHA-256 is
`a6ef7cd2bde7116856a96dbc735a13ed6a918115346c537c5e0971ec0d24474d`.

- The two-adapter Blueprint used the historical index selector, then passed the
  selected clause to `monolith_fields_hi24` for four-field extraction.
- The five-adapter baseline used the index selector and the historical quantity,
  item, city, and unit-price adapters. This is the indexed variant that scored
  76/80 in the historical experiment.
- The single-adapter baseline sent the full message to `monolith_fields_hi24`,
  then calculated prices in Python. Reusing the extractor weights in this
  baseline and the two-adapter Blueprint isolates the effect of giving the
  extractor a selected clause.

The first two variants checked whether the selected clause supported the
extracted fields before pricing. All three used the same arithmetic. Exact
match compared all four extracted fields and the three computed prices. Invalid
counts record the stage that stopped the run. A stage mismatch is a value that
decoded successfully but differed from the corresponding expected value.
Mismatches across stages can overlap.

The benchmark ran on Linux on an Intel Core i5-12450HX with four Torch threads,
Torch 2.14.0, Transformers 5.16.1, and PEFT 0.20.0.
Each variant started in a fresh process and reused one base checkpoint across
its 80 cases. Cold latency includes the first model load and first order. Warm
latency is the median for the remaining 79 orders. Timing includes adapter
selection and Python validation. SQLite persistence and HTTP transport are
outside the timed code. Linux filesystem cache was left in its usual state.
RSS is sampled process memory. CUDA was unavailable for this run.

## Real model results

| Variant | Exact match | Invalid | Cold first order | Warm median | Peak RSS |
| --- | ---: | ---: | ---: | ---: | ---: |
| Two adapters | 77/80 | 3 | 5.093 s | 1.272 s | 1,647 MiB |
| Five adapters | 76/80 | 4 | 4.998 s | 1.396 s | 1,689 MiB |
| Single adapter | 75/80 | 0 | 4.583 s | 0.837 s | 1,663 MiB |

All invalid outputs in the two- and five-adapter variants failed at the pricing
step when clause grounding failed. The selector chose the wrong clause on three
cases in both variants. The four-field extractor returned three mismatched
records given the selected clauses and five given the full messages. In the
five-adapter run, the quantity, city, and price stages each had three mismatches,
and the item stage had four. The measured accuracy lead was one case over the
five-adapter baseline and two over the single adapter.

The authenticated backend was also exercised with the saved starter graph,
real model bindings, and live inference. A separate mixed-checkpoint workflow
used SmolLM2-135M to select clause 2 and Qwen3-0.6B to compute a typed JSON result
of 393. The API trained and bound both starter adapters in short training runs.
The benchmark used the historical weights described above.

## Fixture verification

The two order fixtures passed with their declared expected outputs.
The offline suite also verified a fixture with an out-of-range clause index
that stopped at lookup. These fixture cases are independent of the 80 real
generations per variant.

## Reproduce

Supply the checkpoint directory, a directory containing the historical PEFT
adapters, and the held-out case file. Each JSONL case needs `message`,
`active_clause`, the four expected fields, and `subtotal`, `shipping`, and
`total`.

```bash
OMP_NUM_THREADS=4 MKL_NUM_THREADS=4 HF_HUB_OFFLINE=1 PYTHONPATH=backend/src \
  .venv/bin/python backend/tests/live_blueprint_benchmark.py \
  --model /path/to/checkpoint --adapters /path/to/adapters \
  --cases /path/to/test.jsonl --output /path/to/report.json
```

The archived graph, sample inputs, and training examples are available in the backend test fixture.
