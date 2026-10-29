# Storage, privacy, and backup

## Where data lives

| Data | Location / behavior |
| --- | --- |
| Linux application data | `~/.magnite`, overridden by `MAGNITE_HOME` |
| Saved data-directory preference | Applied on next desktop start; MAGNITE_HOME takes precedence; does not move existing data |
| Registry and datasets | SQLite databases and normalized dataset files under application data |
| Training jobs and adapters | Durable job history and adapter files under application data |
| RAG | SQLite/NumPy index under the application data `rag/` directory |
| Adapter imports/exports | Application data `adapters/imports/` and `adapters/exports/` |
| vLLM logs | Application data `vllm.log` when that engine is used |
| Hugging Face weights | huggingface_hub cache, separate from MAGNITE_HOME |
| Qt preferences | QSettings under organization Magnite, application Magnite LLM; on Linux normally under XDG_CONFIG_HOME or `~/.config` |
| Chat | Current conversation is in memory; durable chat save/restore is not implemented |

The registry uses the Hub library's cache location: `HF_HUB_CACHE` (exact path),
then legacy `HUGGINGFACE_HUB_CACHE`, then `HF_HOME/hub`, then
`XDG_CACHE_HOME/huggingface/hub` or `~/.cache/huggingface/hub`. Set environment
variables before starting the app. Refreshing the model library discovers
existing base-model snapshots without downloading them. See the
[backend process map](development/BACKEND_PROCESS.md) for selection and completeness rules.
The code has a Windows APPDATA path branch, but Windows behavior is not verified.

## Network activity

Model searches, model downloads, authentication, and initial embedding downloads
may contact Hugging Face and its download infrastructure. Ollama operations use
MAGNITE_OLLAMA_URL, which is loopback by default but can point elsewhere. vLLM is
started locally. The desktop-to-backend connection uses loopback HTTP/SSE.

The app has no telemetry implementation. Local files and prompts are used for
local training/inference/RAG; a configured remote Ollama service changes where
its inference happens. This is not a guarantee that third-party libraries never
make network requests. An offline workflow requires cached dependencies and
weights, and should be verified with the intended engines.

No encryption-at-rest feature is implemented. The backend runs with the user's
filesystem permissions. Requests to an app-launched backend need a per-launch secret
(verified on macOS only); a backend started by hand has no credential check.
Keep it local and use operating-system account/storage protections as needed.
Logs and exported adapter metadata may contain local paths, model identifiers,
or error text. Inspect and redact them before sharing.

## Deletion and backup

Deleting a Hugging Face registry model uses cache-aware revision deletion; shared
blobs may remain because another revision references them. This also applies to
models automatically discovered from other applications' downloads: deletion
removes the selected shared-cache revision, not just the Magnite listing. Deleting an Ollama
model requests deletion from Ollama, not merely hiding it in Magnite. Active
inference/training references can block model deletion.

Deleting an indexed RAG document removes its indexed content; the original source
file remains. Dataset ingestion makes normalized copies. Adapter deletion does
not replace the need to manage separately saved export archives or backups.
Deletion is not a secure erase of storage media.

For a consistent backup, stop jobs, unload inference, close the app/backend, then
copy the entire application data directory and the Qt settings directory. Back up
original datasets/documents and needed model caches separately. Restore with the
same application revision and path layout first; records can contain absolute
paths and automatic cross-version/path migration is not established. Changing
the storage setting alone does not migrate a store.
