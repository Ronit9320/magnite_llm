# Training page and live visualisation

Implemented on local branch `feature/training-visualisation`, starting from `dev`.
The supplied `design/pages/training_page` PNG is the layout reference (the design
folder is local and ignored by Git). This work remains local for testing before a
later merge into `dev`.

## Page layout

The page uses two columns. The left side contains numbered cards for base model,
dataset, training settings, and adapter output. The right side contains the run
setup, active run and charts, saved runs, and an expandable event log. Settings
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
- **Start Training** locks setup and past-run selection until the job is terminal.
  **Stop** requests cooperative cancellation; it does not pretend to pause or
  support resuming an optimiser checkpoint.
- Select a past run to inspect its persisted curves, metrics and result message.
  Refresh/reopening the page reattaches to an existing queued or running job.

The sketch's cloud fine-tuning mode is outside the local release scope. Pause,
VRAM prediction, adapter-size prediction and pre-run duration prediction are not
implemented. The run setup shows recorded model disk size and dataset metadata;
disk size is not presented as required VRAM. The summary tooltip explains that
VRAM/duration estimates are unavailable.

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
chart dependency. Samples are sorted by step and placed using their true step
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
Generated screenshots/logs live under ignored `build/`.

Verified on Linux for this implementation:

- CMake configure/build passed with no compiler warnings in the final build.
- Backend suite: **209 tests**, **208 passed**, **1 platform-specific skip**.
- Theme audit: **0 failures**.
- UI checks at normal and 2× scale: **0 failures** each.
- Desktop backend startup/selftest: **0 failures**.
- Real offline CPU LoRA smoke: **6 training samples**, **2 validation samples**,
  successful adapter registration.
- Populated dark/light captures inspected; see
  `build/training-ui/training-running-full.png` and
  `build/training-ui/training-running-full-light.png`.

Native Windows, macOS, CUDA QLoRA, long-run memory behavior and training quality
require further manual testing on the corresponding hardware before promotion.
