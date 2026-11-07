# Training page and live visualisation

Implemented on local branch `feature/training-visualisation`, starting from `dev`.
The supplied `design/pages/training_page` PNG is the layout reference (the design
folder is local and ignored by Git). This work remains local for testing before a
later merge into `dev`.

## Page layout

The page uses two columns. The left side contains numbered cards for base model,
dataset and training settings. The adapter name is under expandable Output options. The right side contains a resource estimate, active run and charts, saved runs, and an expandable event log. Settings
remain visible and scroll when necessary. Both existing themes, keyboard model
selection and reduced-motion preferences remain supported.

- Choose an imported dataset, or use **Import file…**. Its recorded format and row
  count appear below the selection.
- Select LoRA with quantization **None**, or enable 4-bit QLoRA on supported
  installations. The existing capability probe disables unsupported quantization.
  Training needs original Transformers weights; GGUF weights are for inference.
- Rank, alpha, dropout, epochs, learning rate, batch size, target modules, maximum
  token length and warmup ratio are editable. Defaults preserve existing training
  behavior: no validation, dropout 0.05, rank 8, alpha 16 and three epochs.
- **Save preset…** exports settings to JSON using an atomic save. **Load preset…**
  restores settings while idle. Existing session settings remain compatible.
- **Start Training** locks setup until the job is terminal. Past runs remain
  available for review while training continues.
  **Stop** requests cooperative cancellation; it does not pretend to pause or
  support resuming an optimiser checkpoint.
- Past runs are compact rows with a name and result, rather than serialized job
  records. Click a row (or activate it using the keyboard) to open run review.
  Refresh/reopening Training reattaches to an existing queued or running job.
- Visualisation starts enabled on app startup. Turn it off to show the raw job
  JSON, including configuration, history, timestamps and errors. This switch is
  available in both Training and run review and does not alter captured data.

The sketch's cloud fine-tuning mode is outside the local release scope. Pause
and checkpoint resume remain unsupported; Stop uses cooperative cancellation.

## Contextual run review

`TrainingRunPanel` is a page inside the workspace, with no new sidebar entry.
Training's past-run rows and the **View run data** button on locally trained
adapter cards both open this page with a training job ID. Imported adapters
without a linked training job do not show this button. Back returns to the page
that opened review; selecting a sidebar destination exits review normally.

Review contains a run overview, training/validation loss, learning rate,
gradient norm, configuration and timing. It also has a raw-data switch. Active
runs refresh once per second while review is visible; terminal runs stop
polling. Missing/deleted run history is explained in the page. The existing
six sidebar panels remain unchanged.

## Resource estimates

`POST /training/estimate` accepts a `TrainingConfig` and reads the selected
imported dataset and local model configuration. It never downloads weights or
loads an ML runtime. Changes to the inputs debounce estimate requests; stale
replies are ignored. The card shows an approximate memory range, available
memory, expected adapter weight size, optimiser steps and a duration range
when local timing calibration exists.

The budget supports common Llama/Mistral/Qwen/Gemma and GPT-2 configurations.
It approximates base projection/embedding parameter counts from model dimensions
and LoRA parameters from rank and selected projections. Unknown target names
use an all-linear budget, matching the trainer fallback. Base weights are
budgeted as FP32 for LoRA, or quantized weights with overhead for QLoRA. The
adapter estimate assumes FP32 storage; optimizer/gradient and activation budgets
are added to memory, with a range for allocator/kernel/attention uncertainty.
These are advisory architecture budgets, not a guarantee that a run will fit.
Missing/unsupported configuration is reported explicitly rather than treating
model download size as VRAM.

Available NVIDIA memory is read with `nvidia-smi` using one GPU's capacity,
never the sum of multiple cards. Without that probe the card reports Linux or
Windows system RAM; macOS reports unified/system total memory with available
memory marked unknown. Memory labels distinguish RAM from VRAM. This probe is
an advisory machine capacity reading; it does not choose the trainer's device.

Pre-run duration needs a completed local run for the same model, batch size,
sequence length, quantization, rank and target modules, with at least two timed
training samples. The range scales measured step time to the selected workload
(0.7–1.5×), excludes initial loading/final saving and may change with hardware or
concurrent work. Without calibration the card says it calibrates after a
matching run. No fixed GPU speed or invented completion time is used.

## Raw telemetry and charts

The backend remains responsible for ML and supplies `TrainingJob.history` through
`GET /training/status/{id}` and `GET /training/jobs`. The client polls once a
second, with at most one status request in flight. Every response replaces the
visible history rather than appending duplicate points from repeated polls.

Each retained history entry has its actual optimiser `step`, an
`elapsed_seconds` measurement from training start, an `epoch` when available,
and the scalar values actually logged by Transformers:

| Metric | Meaning and display |
| --- | --- |
| `loss` | Logged training loss; solid orange line on the Loss tab |
| `eval_loss` | Held-out evaluation loss; dashed green line on the Loss tab |
| `learning_rate` | Optimiser learning rate; separate tab |
| `grad_norm` | Gradient norm when supplied by the trainer; separate tab |

`TrainingChart` uses Qt's painter and existing theme tokens, with no additional
chart dependency. New history reveals measured line segments over 550 ms with
an eased animation; opening a chart reveals its retained curve. Unchanged polls
do not replay the animation. Hidden charts stop their animation, completed
animations settle, and reduced motion displays samples immediately. No artificial
loss samples or continuous motion are introduced. Samples are sorted by step and placed using their true step
coordinates. Axis bounds follow the measured values; the x-axis extends to the
known total steps. A single sample is drawn as a point. Hover over the plot to
read exact measured values at the nearest recorded step. Accessible descriptions
include the sample count and latest measurement. The text summary remains
available alongside the graphs.

Missing/null/non-finite values are omitted, rather than plotted as zero. The
backend does not turn final training summary logs into extra loss samples. Zero
is a valid measured value. No synthetic, smoothed or forecast loss is displayed.
The current `job.loss` remains the backend's final aggregate training loss after
completion; the plotted final training sample is the final logged step loss.
These values can differ.

History retains the most recent **1,000 metric entries**, including evaluation
entries. Old runs may lack new timing, epoch or validation fields and still
render the measurements they have. The charts display the retained window, not
an unbounded full-run archive. Terminal history is persisted by the existing job
manager. There is no database format migration or release/schema identity change.

The summary shows steps, epoch, train/validation loss, learning rate, gradient
norm and elapsed time. Approximate remaining optimiser time and steps/second
require two timed training samples with increasing steps. The calculation uses
sample differences, excludes initial model loading, and is unavailable while
queued/saving/terminal or without sufficient data. Evaluation overhead between
samples can affect this approximation; it is not a promised completion time.

Transient status/network failures retain the tracked job and measurements,
keep configuration locked and retry. A missing job (404) is surfaced as an
interruption. Submission and cancellation failures remain visible and
recoverable. Stale list/cancellation replies are ignored when a newer operation
has superseded them.

## Held-out validation

`TrainingConfig` adds two optional, backward-compatible fields:

- `dropout`: LoRA dropout, default `0.05`, range `0 <= x < 1`.
- `validation_split`: fraction of rows to hold out, default `0`, range `0..0.5`.
  The UI displays this fraction as `0..50%`.

A nonzero split requires at least two rows and is rejected before queuing when
that condition is not met. Tokenized rows are split with the configured seed;
the test set uses `ceil(rows * validation_split)` rows, bounded so at least one
training row remains. Validation rows are excluded from the training dataset.
The trainer evaluates at each epoch boundary, including its final partial epoch,
and emits `eval_loss` at the actual optimiser step. With validation disabled,
there is no evaluation dataset, extra evaluation cost or invented validation
curve. Dropout is passed directly to `LoraConfig`.

`on_train_begin` takes total steps from the trainer's actual state. Step progress
updates on every optimiser step, independently of loss logging. Timing samples
use a monotonic clock; the elapsed run summary also uses job lifecycle timestamps
so it remains meaningful after reattaching to a persisted run.

## Verification

Run from the repository root:

```bash
cmake -S app -B build
cmake --build build -j2
PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests -v
QT_QPA_PLATFORM=offscreen ./build/magnite-app --theme-check
QT_QPA_PLATFORM=offscreen ./build/magnite-app --ui-check build/training-ui
QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=2 ./build/magnite-app --ui-check
PYTHONPATH=backend/src .venv/bin/python backend/tests/live_training_visualisation.py
```

The final command is an opt-in offline CPU smoke test requiring `backend[ml]`
and an already cached `sshleifer/tiny-gpt2`. It uses disposable datasets/adapters,
does not download weights, and asserts six real training samples, two validation
samples at steps 3/6, measured learning rate/gradient/epoch/timing and successful
adapter registration. It writes `build/training-real-telemetry.json` when the
build directory exists. Tiny-model success verifies wiring, not model quality.

Offline regressions cover finite/missing metrics, actual step numbering, bounded
history, distinct train/validation values, immutable list snapshots, persisted
history, seeded splitting, configuration bounds and rejecting one-row validation.
UI checks exercise historical curves, live charts, repeat-poll deduplication,
empty telemetry, HTTP outages, cancellation, submission recovery, completion,
reattaching to active jobs, compact layout, keyboard navigation and both themes.
Additional checks cover run review from Training/Adapters, raw-data switching,
animation frame changes/settling/reduced motion, resource budgets and measured
duration calibration.
Generated screenshots/logs live under ignored `build/`.

Verified on Linux for this implementation:

- CMake configure/build passed with no compiler warnings in the final build.
- Backend suite after the polish update: **216 tests**, **215 passed**, **1 platform-specific skip**.
- Theme audit: **0 failures**.
- UI checks at normal and 2× scale: **0 failures** each.
- Desktop backend startup/selftest: **0 failures**.
- Real offline CPU LoRA smoke: **6 training samples**, **2 validation samples**,
  successful adapter registration.
- Populated dark/light captures inspected; see
  `build/training-polish-ui/training-running-full.png`,
  `build/training-polish-ui/training-running-full-light.png` and
  `build/training-polish-ui/training-run-review.png`.

Native Windows, macOS, CUDA QLoRA, long-run memory behavior and training quality
require further manual testing on the corresponding hardware before promotion.
