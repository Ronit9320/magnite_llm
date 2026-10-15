# Local workflows

These instructions describe the current app. Use the [example inputs](../examples/README.md)
for a small walkthrough; they are format demonstrations, not a useful training corpus.
The sidebar calls Models “Library,” Chat “Playground,” and RAG “Knowledge.”

## Download, load, and chat

1. Open Models, search a Hugging Face repository, select a result, and Download.
2. Wait for completion, then refresh Local Models if necessary.
3. Open Chat, select the downloaded model, choose Auto or a compatible engine,
   and load it. GGUF uses llama.cpp; transformer weights use torch/vLLM.
4. Send a short prompt. Enter sends; Shift+Enter inserts a newline.
5. Stop generation before switching models. Unload to release the inference model.

Auto respects the global engine override. If an incompatible override was saved,
choose an explicit compatible engine or reset Settings to Auto.

## Prepare data and train

1. Download a compatible transformer base model; do not select a GGUF snapshot.
2. In Training, import `examples/training.jsonl`, `examples/training.csv`, or
   `examples/training.txt`. Select the imported dataset and base model.
3. Start with LoRA and quantization None on a compatible setup. Choose a small
   rank/batch/context configuration appropriate to the model and hardware.
4. Set the adapter name and training parameters, then start. Observe status,
   loss, and steps. A job may queue behind another training job.
5. On success, find the adapter in Adapters. Cancel requests cooperative stopping;
   an active native operation may need to return first.

Supported JSONL records include `{"text":"..."}`, valid role/content message
lists under `messages`, and `instruction`/`input`/`output` records. CSV supports
text and instruction fields. Plain text is normalized into training examples.
UTF-8 BOMs are accepted. Missing or non-string sample values are rejected with row details.
CSV fields are limited to 131,072 characters; larger fields return an import error.
Datasets referenced by queued or running jobs cannot be deleted.
Malformed JSONL is rejected. Review dataset contents and rights before training.
The current automatic target-module fallback may choose `all-linear` when named
modules do not exist; inspect the resulting run rather than assuming a specific
architecture's module names work everywhere.

QLoRA requires a working quantization backend and compatible hardware. The
“8-bit (bitsandbytes)” option selects bitsandbytes 8-bit loading.

## Use, export, and import adapters

1. Activate a completed adapter from Adapters. This restarts inference using its
   base model and PEFT/torch weights; it is not zero-restart native multi-LoRA.
2. Chat and confirm the active adapter is shown.
3. Clear/deactivate the adapter when finished. This unloads inference; load a
   base model again to continue without the adapter.
4. Export an adapter archive and save it. Import expects a Magnite export ZIP
   with metadata and weights. Importing an already registered ID is rejected.

Exported adapters do not include the full base model. The matching model must
remain accessible. Keep a copy of the export before deleting the adapter; do
not delete your only working copy merely to try the import flow.

Adapter imports require a valid LoRA configuration and `adapter_model.safetensors`.
Pickle (`adapter_model.bin`) imports are rejected. Exports validate the source files
and produce a new immutable download URL each time. Adapter metadata retains the
resolved base snapshot path; that snapshot must remain accessible for activation.

## Add documents and use RAG

1. Open Knowledge & retrieval and Add Documents. Try `examples/observatory.md`.
2. Wait for embedding/registration. First use may download `all-MiniLM-L6-v2`.
3. Set chunk size and overlap in **words**, with overlap smaller than size, and
   choose top-k. Some API schema descriptions still incorrectly say tokens.
4. Search “When is the observatory library open?” in the query laboratory.
   Inspect the retrieved passage: the example says Tuesday and Thursday, 18:00–21:00.
5. Enable document retrieval in Chat and ask the same question with a loaded model.
6. Compare the response with the retrieved passage. Retrieval success does not
   guarantee that a small/random model will answer faithfully.

Text PDFs are supported; OCR for scanned PDFs is not implemented. The SQLite/NumPy
store is local. Changing the embedding model while documents exist is blocked;
there is no automatic reindex migration. Removing a document deletes its indexed
representation, not the source document on disk.

For a short demonstration sequence, see the [demo runbook](release/DEMO.md).
