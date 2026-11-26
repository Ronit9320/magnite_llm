# Blueprints and Tool mode

> **Not in v0.2-preview.** Blueprints and Tool mode are still in development and are
> switched off in the magnite-spacev0.2 packages. This guide describes builds with
> them switched on, such as the `dev` branch.

**Conversation** has three modes. **Chat** lets you test a local model and
its active adapter with ordinary messages. Activate trained weights in
**Library → Adapters** and check the active adapter under **Model settings**.
**Tool mode** uses a loaded local model through Pi for an ad hoc task.
**Blueprint** runs a saved workflow and shows its inputs, progress, approvals,
and result.

Open **Blueprints** from the desktop sidebar to build a workflow from typed steps.
**Create new** starts an empty workflow. **Request triage example** creates an
editable draft that summarizes a request and returns a structured review record.

1. Open **Add node** on the canvas. Choose a built-in card or open **Saved
   nodes**, then configure the operation and ports. The
   [node catalog](development/blueprints/NODE_CATALOG.md) lists the available
   operations. Click an output dot and then a matching input dot, or use
   **Connect** to select both ports. Expand a node in **Workflow nodes** to
   see incoming and outgoing links. Selecting a link in the tree reveals the
   node at its other end.
2. For a Model or Reasoning node, choose a downloaded Transformers checkpoint
   in **Configure**. Use **Change binding** for the system prompt, response
   format, and output token budget. A Reasoning node requires a checkpoint
   with explicit thinking support. To reuse trained weights, configure an
   **Adapter** node and connect it to the checkpoint node's **LoRA adapter**
   input. Existing workflows can also use a direct adapter binding.
3. Choose **Save revision** to publish the graph in the local library. Saved
   revisions record the graph and selected binding IDs. To train an adapter, open
   **Training jobs…** on a saved Model or Reasoning node and add JSON examples
   with named `inputs` and `outputs`. When a job completes, select the new
   weights on the Adapter node. For a direct binding, **Bind completed adapter** creates a new saved
   revision.
4. Open **Test & run**. An isolated node test accepts inputs keyed by port
   name. A full run accepts a JSON object keyed by Input node ID. Both accept
   plain text when there is only one string input. **Run history** reopens a
   run and its trace, even if the graph has since changed.
5. Choose **Open in Conversation** in the editor, or select **Blueprint** in
   Conversation and pick a workflow. The picker shows each workflow's latest
   saved revision. Use **Save revision** to make a new draft or an edit
   available there. **Open full run** opens the recorded steps in Blueprints.

A Dataset node can supply inline JSON rows or use a dataset registered in
Training. Set the row schema on the Dataset's output port. **Check readiness**
reports missing bindings, incompatible adapters, token budgets, and runtime
requirements.

The [support ticket routing example](development/blueprints/WORKFLOW_EXAMPLES.md)
connects two local Model nodes through a typed JSON transform. It includes a
portable graph, sample tickets with expected results, and fixture and live
tests for trying linked LoRA adapters.

## Assemble a request-triage workflow

Choose **Request triage example** in Blueprints. Select **Summarize request**,
choose a local Transformers checkpoint in **Configure**, then save the
revision. Open **Test & run** and enter a request such as
“Please send replacement labels for order 245.” The run returns a JSON object
with a model-written `summary` and a fixed `status` of `Needs review`. Each
step shows its inputs and outputs in the run trace. The saved revision can also
run from Blueprint mode in Conversation.

| Step | Input | Output |
| --- | --- | --- |
| Incoming request | Text supplied at run time | Text on `value` |
| Summarize request | The text on `value`, plus the system prompt saved on the Model node | Text on `value` (Plain text response) |
| Review status | A JSON string saved on the Constant node | Text on `value` |
| Build triage record | Text on `summary` and `status` | JSON object with those two keys |
| Triage record | The assembled object | The workflow result |

Model, Reasoning, and Build an object nodes let you add named data ports. In
**Configure**, the key is used in connections, JSON and node tests. The label is
shown on the canvas. Each input has one source, and an output can feed several
inputs. An integer output can feed a number input. Use the inspector to refine
schemas for arrays and objects, including item types and required properties.
For a Model or Reasoning node with multiple outputs, ask for a JSON object
keyed by those output keys. Plain-text response mode uses one text output.

A **Reasoning** node uses the same checkpoint picker as a Model node. Only
checkpoints whose chat template supports explicit thinking can be selected;
others are listed as “no thinking”.

The Pi Agent node was removed. Older graphs that contain one still open, and
the node shows a validation error until you replace it with a Model node that
uses **Plain text** responses and holds the instructions in its system prompt.
The Tool approval node handles explicit actions. Workflows run from
**Test & run** or Blueprint mode in Conversation.

## Supported runtimes and checkpoints

The results below refer to local inference in the desktop. A supported engine
can still produce text that fails a node's schema or Pi's structured response
format. Exact acceptance evidence and platform coverage are in the
[Blueprint acceptance record](development/blueprints/ACCEPTANCE.md).

| Engine or checkpoint | Ordinary chat | Blueprint Model and Reasoning | Tool mode |
| --- | --- | --- | --- |
| Torch/PEFT with SmolLM2-135M-Instruct | Local text | Model with a trained LoRA verified on Linux CPU. Its template lacks explicit thinking. | Eligible when loaded and Pi is available |
| Torch/PEFT with Qwen3-0.6B | Local text | Reasoning with a typed JSON result verified on Linux CPU | Real ad hoc Pi reply verified on Linux CPU |
| llama.cpp with GGUF | Local chat and Playground | Use a Transformers checkpoint for Model and Reasoning nodes | Eligible when loaded and Pi is available |
| Local vLLM | Local text | Use Torch/PEFT for Model and Reasoning nodes | Eligible when loaded and Pi is available |
| Ollama | Local text | Use a Transformers checkpoint for Model and Reasoning nodes | Use a supported loaded engine for Pi |
| Package without optional ML dependencies | GGUF with llama.cpp | Input, Dataset, Transform, Output, and fixed approval steps are available. Use a package with ML support for Model, Reasoning, or adapter training. | Pi can use a supported loaded GGUF model |

Use a complete unquantized Safetensors checkpoint for a decoder-only
Transformers model, including its local tokenizer files.
Adapters need PEFT LoRA weights with the same base checkpoint, compatible
tokenizer, and no shared bias. The readiness check estimates required memory
from the checkpoint weights, device, and output budget. If the estimate exceeds
available memory, choose a smaller checkpoint or lower the output budget. Leave
enough context for the input and output together. Blueprint generation releases
its checkpoint when the run ends. The runner can hold up to eight active or
queued runs and executes one at a time. Linked dataset snapshots are limited to
16 MiB per run. Step trace values are limited to 32 MiB per run.

## Tool requests and approvals

Choose **Tool mode** in Conversation and load a supported local model from the
model toolbar. Send a task and follow up in the same conversation, or choose
**New task** for a fresh context. **Task history** opens a recent turn.
If you run Magnite from source, install Node 22.19 or later
and the [locked Pi dependencies](development/blueprints/PI_INTEGRATION.md#development-runtime).
Native packages that include Pi ship their own Node runtime.

A tool call appears inline in Tool mode and in the Blueprint run that requests
it. Review the exact target or the resolved executable, argument list, and
working directory. **Approve once**
allows that specific action for this run. **Deny** rejects it. An approval
expires after 90 seconds. A new proposal, including a repeated request for the
same action, needs a separate decision. The run can be cancelled while waiting.
You can switch between chat and **Open full run** while a decision is pending.
If one reply asks for several actions, each gets its own card and approval, one
after another, in the order asked. An exact repeat in the same reply is asked
once. Denying one still asks about the rest; cancel the run to stop it.

**Seeing what happened.** A finished command or read card shows what ran and an
**Output** box with what it produced, plus the exit code when a command failed.
Compare the answer with it: a model can misreport what a command printed. The
cards keep this in memory only, so it is gone after the app closes. Output can
hold sensitive details such as serial numbers or file contents, so check
screenshots before sharing them.

**Writing files.** A tool can create a text file or replace one. The card says
**Create file** or **Replace file**, shows the resolved path and the exact
content, and warns before replacing. The folder must already exist, content is
limited to 16,000 bytes, and Magnite's own data folder is protected apart from
`tool-workspace/`. One model reply is capped, so in practice a model can only
write short files, roughly 2–3 KB. There is no partial edit.

**Attempts that did not run** appear as **Not run** cards with the reason, such
as a missing file or a command that needs a shell (`|`, `>`, `<`, `&`, `;`).
They are kept only until the app closes. An answer from a task where nothing ran
is marked **No actions ran**: small models sometimes describe results they never
produced, so check that line and the cards. Very small models often miss steps;
Qwen3 1.7B or larger is recommended.

Opening a target uses the operating system's file, folder, or HTTP(S) handler.
Reading returns a file's text or a folder listing to the run. Approved commands
run with your desktop account's permissions, with a direct argument list and a
30-second time limit. Their output returns to the run and the model. A command
can change files or start other programs. Review all its arguments and deny
requests you don't intend to run. The default working directory is
`tool-workspace/` inside the app data folder. A task has at most six model
turns and four actions.

Run history keeps input values, output values, revisions, and a redacted
decision record. Treat it as local user data when you move, back up, or share
the profile. See the [storage guide](STORAGE_AND_PRIVACY.md) and
[local API](LOCAL_API.md#tool-chat-and-approvals).
