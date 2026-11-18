# Blueprint workflow examples

## Support ticket routing

This example takes a support message, extracts three fields, validates the
result, and hands the typed record to a second model to assign a queue and
priority. The output keeps the extracted record alongside the routing decision
so a reviewer can check both stages.

```text
Input → Model A → Parse JSON → Model B → Build an object → Output
Extraction LoRA → Model A
Routing LoRA → Model B
Parse JSON → Build an object (ticket field)
```

| Node | Contract |
| --- | --- |
| Read support ticket | Run input `ticket` → text |
| Extract ticket fields | Ticket text → `raw_json` text |
| Parse ticket JSON | Raw text → object with `category` (text), `blocked` (boolean), and `reference` (text) |
| Assign queue and priority | Parsed object on `record` → text ports `queue` and `priority` |
| Build routing record | Parsed ticket, queue, and priority → one object |
| Return routing record | Object → run result on `result` |

Model A uses **Plain text** response mode so the Parse JSON node can inspect and
validate its reply. Use Model A's **Typed JSON** response mode and an object output
if you want the model node to do that parsing itself. The schema checks field
names and types. The allowed category names and routing policy are in the
prompts, and the case assertions check the actual decisions.

The test messages are synthetic examples of requests a support team handles.

| Scenario | Expected queue | Expected priority |
| --- | --- | --- |
| Invoice INV-418 was charged twice and service is still working | `finance` | `normal` |
| A backup code fails and an agent cannot continue work | `identity` | `high` |
| Shipment SHP-290 shows delivered, but production is waiting for the parts | `fulfillment` | `high` |
| A customer asks for the refund status on order ORD-714 | `finance` | `normal` |
| A secondary login fails while the main account still works | `identity` | `normal` |
| Order ORD-592 has a cracked case and the equipment still works | `fulfillment` | `normal` |

The duplicate-charge case expects this value at `results.result`.

```json
{
  "ticket": {"category": "billing", "blocked": false, "reference": "INV-418"},
  "queue": "finance",
  "priority": "normal"
}
```

### Build and run it

The [portable graph](examples/support-triage.json) defines every port, link,
and prompt. Recreate it using **Blueprints → Create new**, or replace its
model and adapter IDs with compatible IDs from your profile and submit it to
[`POST /blueprints/import`](../../LOCAL_API.md#blueprints). The example uses
Qwen3-0.6B for both Model nodes. Use the full local Transformers checkpoint.
The two adapter IDs in the file are placeholders. Use
`GET /blueprints/checkpoints/{model_id}` to find the adapter IDs compatible
with a registered checkpoint.

Connect each Adapter node to the corresponding Model's **LoRA adapter** input.
To reuse one adapter, connect its output to both models and remove the second
Adapter node. Both models need checkpoints compatible with the shared weights.
Each model still uses its own prompt, response format, and port contracts.
To use a different checkpoint for Model B, select an adapter trained for that
checkpoint and keep the typed input contract on `record`.

In **Test & run**, first test **Parse ticket JSON** with an isolated input
such as `{"value":"{\"category\":\"billing\",\"blocked\":false,\"reference\":\"INV-418\"}"}`.
Then try truncated JSON, a string `"false"` in place of the boolean, and a
missing `reference`. Each bad value should fail at Parse JSON. In a full run,
the downstream routing and Output nodes should be skipped. Finally, save the
graph, choose **Full blueprint** under **Test & run**, and paste a ticket from the
[case file](examples/support-triage-cases.json). Compare the Parse JSON output,
Model B input, and final result in the trace.

### Repeat the checks

The offline test uses the portable graph, substitutes fixture model responses,
and checks the six expected results plus the three invalid handoffs.

```sh
PYTHONPATH=backend/src:backend/tests .venv/bin/python -m unittest \
  test_blueprints.BlueprintApiTests.test_support_triage_example_and_invalid_json_handoff -v
```

The [live test](../../../backend/tests/live_blueprint_support.py) uses
`backend[ml]` and the cached Qwen3-0.6B Safetensors snapshot at revision
`c1899de289a04d12100db370d81485cdf75e47ca`. It trains two LoRAs in a temporary
profile using the separate [training examples](examples/support-triage-training.json),
then compares the same held-out tickets with separate LoRAs and with the
extraction LoRA shared across both Model nodes. It checks which adapter each
node received and reports completed runs and exact matches.

```sh
OMP_NUM_THREADS=4 MKL_NUM_THREADS=4 HF_HUB_OFFLINE=1 PYTHONPATH=backend/src \
  .venv/bin/python backend/tests/live_blueprint_support.py
```

### Results on Linux CPU

All six fixture tickets produced their expected results. The truncated JSON,
wrong boolean type, and missing reference each failed at Parse JSON and
skipped routing.

Both live variants completed every run and delivered a schema-valid record.
Each LoRA trained for 12 steps on its own task. The shared variant used the
weights trained for extraction at both nodes.

| LoRA setup | Completed | Exact extraction | Exact final record |
| --- | ---: | ---: | ---: |
| Separate extraction and routing LoRAs | 6/6 | 5/6 | 3/6 |
| Extraction LoRA shared across both models | 6/6 | 5/6 | 0/6 |

Both variants read the refund-status ticket as delivery. With separate LoRAs,
the two access tickets had the correct priorities and returned queue
`access` where the policy calls for `identity`. With the shared LoRA, Model B returned
`finance` and `high` for all six tickets. This run checks the adapter links
and measures task behavior on six synthetic cases. The live command exits
with status 1 when any final record differs from the expected value.

## More workflows to exercise

- **Order intake.** Start with the built-in order example. A customer cancels
  an old order and requests six items at $3 each. The selected clause should be the new
  order. With $5 shipping, the total should be $23. The
  [order benchmark](ORDER_BENCHMARK.md) records real adapter results and invalid
  clause selections.
- **Request triage.** Start with the built-in Pi example with a local model
  loaded in Chat. “Please send replacement labels for order 245” should produce
  a summary that keeps the requested action and order number, plus a
  `Needs review` status.
- **Invoice review.** Reuse the two-model pattern with an email that includes
  invoice ID, amount, currency, and purchase-order reference. Give the second
  model the typed invoice and a prompt to flag missing references for review.
  For a rule-based version, Validate data checks the extracted invoice, Compare
  tests the amount against a Constant limit, Choose a value picks
  `Needs review` or `Approved`, and Fill a text template writes the review note
  without another generation. The offline test
  `test_template_compare_choose_and_validate_route_a_typed_record` runs this graph.
