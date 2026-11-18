# Blueprint node catalog and next additions

The **Add node** dialog shows nine built-in node cards. Choose a card, then
choose an operation in **Configure**. **Saved nodes** contains reusable copies
of configured nodes and opens additional pages as the library grows.

## Available now

| Card | Operation | Input and output |
| --- | --- | --- |
| Input | Read run input | Takes a value supplied when the run starts and emits it on `value`. |
| Dataset | Provide JSON rows | Reads inline JSON or a dataset registered in Training and emits a typed `rows` array. |
| Transform | Pass through | Takes a typed `value` and forwards it unchanged. |
| Transform | Parse JSON | Takes JSON text and returns a value of the selected type. |
| Transform | Get object field | Takes an object and returns the value of one named field. |
| Transform | Get first item | Takes an array and returns its first item. An empty array fails the step. |
| Transform | Count items | Takes an array and returns an integer count. |
| Transform | Format as text | Takes a value and returns text. Structured values become JSON text. |
| Transform | Provide a constant | Emits a fixed JSON value saved on the node. |
| Transform | Build an object | Takes named, typed inputs and returns an object with those input keys. |
| Model | Generate a response | Takes named inputs and produces typed JSON or a single text output using a local Transformers checkpoint. |
| Reasoning | Reason through a task | Takes named inputs and produces a typed final answer using a checkpoint with explicit thinking support. |
| Adapter | Attach trained LoRA | Takes a binding to registered LoRA weights and provides a resource link to a compatible Model or Reasoning node. |
| Pi Agent | Respond with Pi | Takes a text task and saved instructions and returns a text reply using the model loaded in Conversation. |
| Tool | Request approval | Takes a structured action and emits a receipt after approval and execution. |
| Output | Return value | Takes a typed value and exposes it in the run result. |

Saved workflows from the retired Order intake starter can still run **Look up
order clause** and **Validate and price order**. Older graphs may also contain
**Ask the local tool model**, a Tool operation that uses Pi with the approval flow.

The [support ticket routing example](WORKFLOW_EXAMPLES.md) shows how Model,
Adapter, Parse JSON, and Build an object compose in one graph. Its test cases
cover typed handoffs and shared or separate adapters.

## Recommended additions

| Priority | Node or operation | Contract and use |
| --- | --- | --- |
| First | Text template | Named values plus a saved template → text. Build prompts, titles, and report sections without model generation. |
| First | Compare | Two compatible values → boolean. Start with equality and numeric comparisons for rules such as an order-total threshold. |
| First | Choose value | A boolean and two already-computed values of the same type → the chosen value. Use it to produce a status or label. |
| First | Validate data | A value and a saved schema → the validated value, or a failed step with the field path. Give the author an explicit validation point before downstream work. |
| Next | Filter and sort rows | A typed array and a configured field rule → a typed array. Use these to prepare a dataset for a model or report. |
| Next | Knowledge search | A text query and selected local knowledge collection → ranked excerpts with source references. Reuse Magnite’s existing retrieval settings. |
| Later | Branch and merge | A boolean selects which path runs. Define skipped-step trace states and how downstream inputs resolve before adding this to the editor. |
| Later | For each | A typed array and a reusable workflow → an array of results. Define iteration limits, cancellation, and per-item error reporting first. |
| Later | Export result | A typed value → a JSON or text file and a receipt. Use a user-selected destination and the existing explicit approval model for external effects. |

For request triage, the first additions would let a Blueprint validate the
incoming record, template a Pi task from named fields, compare the result
against a review rule, and choose the status in its final object. Scheduling
and file-arrival triggers belong to workflow start settings and can be designed
once a saved workflow's inputs and run behavior are stable.
