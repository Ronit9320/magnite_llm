# Blueprint node catalog and next additions

The **Add node** dialog shows eight built-in node cards. Choose a card, then
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
| Transform | Fill a text template | Takes named inputs and returns text. `{key}` inserts an input (structured values as JSON); `{{` and `}}` are literal braces. Every placeholder needs an input port. |
| Transform | Compare two values | Takes `left` and `right` and returns a boolean. Equals and Does not equal use JSON equality (`true` is not `1`). The four ordering comparisons take numbers. Contains looks for text in text or a value in an array. |
| Transform | Choose a value | Takes a boolean `condition` and two values of the output type, and returns `if_true` or `if_false`. Both inputs are computed; this is not branching. |
| Transform | Validate data | Takes a value and saved rules (the JSON Schema subset used by ports: `type`, `properties`, `required`, `additionalProperties`, `items`, `minimum`, `minLength`). Returns the value unchanged or fails the step at the field path, skipping later steps. |
| Model | Generate a response | Takes named inputs and produces typed JSON or a single text output using a local Transformers checkpoint. |
| Reasoning | Reason through a task | Takes named inputs and produces a typed final answer using a checkpoint with explicit thinking support. The checkpoint picker disables checkpoints without it. |
| Adapter | Attach trained LoRA | Takes a binding to registered LoRA weights and provides a resource link to a compatible Model or Reasoning node. |
| Tool | Request approval | Takes a structured action and emits a receipt after approval and execution. |
| Output | Return value | Takes a typed value and exposes it in the run result. |

Saved workflows from the retired Order intake starter can still run **Look up
order clause** and **Validate and price order**. The **Pi Agent** node was
removed; older graphs containing it open with a `retired_node` validation error
until it is replaced with a Model node using Plain text responses. Older graphs may also contain
**Ask the local tool model**, a Tool operation that uses Pi with the approval flow.

The [support ticket routing example](WORKFLOW_EXAMPLES.md) shows how Model,
Adapter, Parse JSON, and Build an object compose in one graph. Its test cases
cover typed handoffs and shared or separate adapters.

## Recommended additions

| Priority | Node or operation | Contract and use |
| --- | --- | --- |
| Next | Filter and sort rows | A typed array and a configured field rule → a typed array. Use these to prepare a dataset for a model or report. |
| Next | Knowledge search | A text query and selected local knowledge collection → ranked excerpts with source references. Reuse Magnite’s existing retrieval settings. |
| Later | Branch and merge | A boolean selects which path runs. Define skipped-step trace states and how downstream inputs resolve before adding this to the editor. |
| Later | For each | A typed array and a reusable workflow → an array of results. Define iteration limits, cancellation, and per-item error reporting first. |
| Later | Export result | A typed value → a JSON or text file and a receipt. Use a user-selected destination and the existing explicit approval model for external effects. |

Text template, Compare, Choose a value, and Validate data are available. A
Blueprint can now validate an incoming record, template a model prompt from
named fields, compare a value against a review rule, and choose the status in
its final object without another generation. Scheduling
and file-arrival triggers belong to workflow start settings and can be designed
once a saved workflow's inputs and run behavior are stable.
