# Blueprints and Tool mode

Blueprints are saved workflows that connect typed steps. Open **Blueprints**
from the desktop sidebar and choose **Create new** or **Order intake example**.
The example is an editable draft. It selects an active clause from an order
message, extracts fields, validates them, and calculates the price.

1. Add nodes in **Canvas** and connect outputs to compatible inputs. Select a
   node to edit its ports or test it with a small example. Tool and Model nodes
   use typed input and output contracts just like other nodes.
2. For a Model or Reasoning node, choose a locally downloaded Transformers
   checkpoint in **Change binding**. You can use the base checkpoint by itself,
   choose a compatible LoRA adapter, or train one for the node. A Reasoning node
   requires a checkpoint with explicit thinking support.
3. Choose **Save revision** to publish the graph in the local library. Saved
   revisions record the graph and selected binding IDs. To train an adapter, open
   **Training jobs…** on a saved Model or Reasoning node and add JSON examples
   with named `inputs` and `outputs`. The order starter supplies examples for its
   model nodes. When a job completes, choose **Bind completed adapter**. That
   creates a new saved revision.
4. Open **Test & run**. An isolated node test accepts inputs keyed by port
   name. A full run accepts a JSON object keyed by Input node ID. Both accept
   plain text when there is only one string input. **Run history** reopens a
   run and its trace, even if the graph has since changed.
5. To run the same saved workflow from a conversation, select **Blueprint** in
   the chat composer and choose the saved revision. **Open full run** opens
   that exact run and its recorded steps in Blueprints.

A Dataset node can supply inline JSON rows or use a dataset registered in
Training. Set the row schema on the Dataset's output port. **Check readiness**
reports missing bindings, incompatible adapters, token budgets, and runtime
requirements.

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

Choose **Tool mode** in the chat composer for an ad hoc local task. First load a
supported chat model. If you run Magnite from source, install Node 22.19 or later
and the [locked Pi dependencies](development/blueprints/PI_INTEGRATION.md#development-runtime).
Native packages that include Pi ship their own Node runtime.

When a run requests an external action, review the exact target or the
resolved executable, argument list, and working directory. **Approve once**
allows that specific action for this run. **Deny** rejects it. An approval
expires after 90 seconds. A new proposal, including a repeated request for the
same action, needs a separate decision. The run can be cancelled while waiting.
You can switch between chat and **Open full run** while a decision is pending.

Opening a target uses the operating system's file, folder, or HTTP(S) handler.
Approved commands run with your desktop account's permissions, with a direct
argument list and a 30-second time limit. A command can change files or start
other programs. Review all its arguments and deny requests you don't intend to
run. The default working directory is `tool-workspace/` inside the app data
folder. Pi has at most five model turns and four proposals per agent node.

Run history keeps input values, output values, revisions, and a redacted
decision record. Treat it as local user data when you move, back up, or share
the profile. See the [storage guide](STORAGE_AND_PRIVACY.md) and
[local API](LOCAL_API.md#tool-chat-and-approvals).
