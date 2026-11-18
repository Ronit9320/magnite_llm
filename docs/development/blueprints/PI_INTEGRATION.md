# Pi tool runtime

Magnite uses Pi Agent Core 1.5.2 for model-driven tool turns. A fixed Blueprint
continues to run through Magnite's graph executor. Its approval node can submit
a fixed action directly. Pi Agent nodes use the model loaded in Chat and pass
their saved instructions as the system message. Their `task` and `reply`
ports carry text. Pi registers an empty tool list for these nodes and the
Python bridge rejects proposal frames. Saved Tool nodes with the legacy
`agent` operation continue to use the same approval flow as tool chat.

Pi's custom `streamFn` exchanges newline-delimited JSON with the Python worker
over stdin and stdout. Pi sequences turns and tool executions. Magnite owns
the prompt, each model turn, reply parsing, and dispatch. This preserves
Magnite's existing HTTP and SSE protocols. A task has at most six model turns
and four actions. On the last turn Pi withholds the tools so the model answers.

For tool chat and legacy Tool agent nodes, the model sees four tools:
`run_command` (one command line, split without a shell), `read_file`,
`list_folder`, and `open_target`. Where the loaded model's chat template
declares tools (Qwen, Llama 3.x, SmolLM3 and similar), the Torch and llama.cpp
engines render each turn with that template and thinking disabled. Other
models get the same tools as a JSON protocol in the system message, and vLLM
receives `enable_thinking: false`. The parser accepts `<tool_call>` blocks,
fenced or bare JSON calls, Python-style calls such as `read_file(path='a.txt')`,
and the earlier `{"action": ...}` objects, and it removes `<think>` blocks.
Each call becomes an exact broker action. The broker controls execution, and
Pi receives a readable result: the outcome, the exit code, and the output.
The agent process uses the same saved run, replayable SSE, and cancel endpoint
on both surfaces.
[Pi Agent Core documentation](https://github.com/earendil-works/pi/tree/main/packages/agent)

OpenCode also supports bounded model steps, configured permissions, and session
cancellation through its SDK. It would need a separate OpenCode service,
provider setup for Magnite's local inference protocol, and interception of
actions so that Magnite owned dispatch. Pi's in-process callback model lets
the bridge supply those boundaries directly through the Python worker.
[OpenCode agents](https://opencode.ai/docs/agents/),
[OpenCode SDK](https://opencode.ai/docs/sdk/),
[OpenCode permissions](https://opencode.ai/docs/permissions/)

## Development runtime

Make Node 22.19 or later available on the backend's PATH and install the locked
dependencies from the repository root.

```bash
npm ci --ignore-scripts --prefix backend/src/backend/tooling/pi
```

The Python wheel includes the bridge and npm manifests. It can run after its
locked npm dependencies are staged beside the bridge. On Linux this was
verified with Node 26.13.1 and a wheel unpacked outside the source checkout.
The native builders install the locked npm packages alongside the installed
bridge and stage the checksum-pinned Node 22.23.5 executable next to bundled
Python. The packaged bridge locates Node there. Each package smoke runs the
bridge with the host Node path removed.
[Node downloads](https://nodejs.org/en/download)

## Approval and execution

Magnite authenticates the tool routes with its normal per-launch credential.
For a pending proposal it displays the full resolved target, or executable,
complete argument list and working directory. Every decision is bound to the
proposal ID, run, node, and fingerprint. The worker claims an approved proposal
once before calling the handler. Denial, expiry, and cancellation before claim
settle the proposal without an external action. Repeating a proposal produces a
fresh ID and requires a separate decision.

Commands use a direct argument list with a 30-second limit. The handler
terminates a running command on cancellation. Combined standard output and
error, capped at 16,000 bytes, go back to the requesting run and model. Read
returns a file's text with the same cap, or a folder listing of at most 500
entries. Desktop open uses the platform handler and reports the dispatch
outcome. The event and audit records omit raw arguments, paths, URLs, and
output. Current pending values are held in memory for exact review. Step
inputs, receipts, and the chat transcript are part of normal run history.
A restart marks an active approval interrupted. If the action had been claimed,
its outcome can be uncertain. The next backend process does not replay it.

See the [local API contract](../../LOCAL_API.md#tool-chat-and-approvals) for
starting tool chat, binding decisions, and following a run after reconnect.

Linux verification exercised Qwen3-0.6B through both tool chat and a saved
Blueprint's Pi node. Both proposed an exact URL and completed after denial.
The broker tests verified real, approved command dispatch once per proposal
in temporary directories. Denial, expiry, cancellation and malformed actions
left the markers untouched. Restart recovery and cancellation of an active
Pi bridge also passed.
