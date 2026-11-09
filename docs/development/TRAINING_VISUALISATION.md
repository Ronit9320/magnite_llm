# Training page and live visualisation

Developed on `feature/training-visualisation`, starting from `dev`, for integration
into `dev`. The [mockup workflow](TRAINING_MOCKUP_WORKFLOW.md) records how the
browser explorations were built and translated into Qt.
The original split view followed the supplied `design/pages/training_page` PNG
(the design folder is local and ignored by Git). Subsequent iterations below
explore alternative native layouts and record the implemented refinements.

## Page layout

The native workspace now follows the Studio and Observatory web explorations,
with the live training studio **first** and the next-run recipe **below it**.
The existing animated Qt dropdowns, buttons, switches, progress bars and chart
reveals remain in use; no web view or web dependencies are added to the client.

- **Studio** is the default view. Its monitor shows run identity, measured
  training/validation loss, measured steps per second and an estimated remaining
  time when enough timed samples exist. Animated metric tabs select loss,
  learning rate or gradient norm. A side card shows the run's saved configuration,
  a recipe reuse action and a device telemetry card. Raw JSON and full review
  are available from the monitor context menu. In compact windows
  this card stacks below the monitor.
- **Next run recipe** follows the studio. Existing model, dataset and method
  dropdowns appear when editing the compact recipe. Edit settings expands hyperparameters and preset/output
  options. The advisory backend resource estimate and Start Training action live
  here too. The next recipe stays editable while training; concurrent starts
  remain disabled and the active experiment keeps its saved configuration.
  Quantization capability checks are unchanged.
- **Observatory** adds simultaneous learning-rate and gradient charts and opens
  the experiment index with two-run comparisons. Selecting a historical run
  shows that snapshot while active polling continues independently; double-click
  opens contextual review.
  Comparisons retain shared axes, actual optimiser steps, saved configurations,
  preserved selections and explicit missing-data handling.
- **Export run** saves the actual job snapshot, including configuration and
  retained history, as JSON. **Use as next run** copies only recorded settings
  into the next recipe. Editing this recipe never changes
  the displayed run's saved configuration.

Submitting returns to Studio, collapses settings and the archive, and scrolls to
its monitor. Stopped/completed charts retain their measurements. Reopening the
page reattaches to queued/running jobs; Stop remains cooperative cancellation.
Device utilization, memory usage, temperature and power come from the new
`GET /training/telemetry` endpoint, without loading ML packages. Unsupported
readings remain unknown, and a system-memory fallback is labelled explicitly.
These are device-wide readings, never attributed to the selected job. There are
no persistent storage changes. Timing figures use recorded training samples.

The native layout follows the captured web references in the local-only
`design/training-preview-reference/`, with native comparisons in
`design/training-native-captures/`. It includes the persistent Observatory index,
inline historical run selection without losing active-job tracking, experiment
table, saved configuration grid, compute bars, activity card, underlined animated
chart tabs, optional EMA display smoothing (raw tooltips/export stay unchanged),
compact next-run recipe and a modal editor using the existing animated dropdowns.
The native sidebar uses the preview's 195px width and compact horizontal brand
header while retaining its collapse animation, shortcuts and saved choice.

Regenerate quick, deterministic design captures without ML downloads:

```bash
QT_QPA_PLATFORM=offscreen MAGNITE_TRAINING_DESIGN_CHECK=1 ./build/magnite-app --ui-check design/training-native-captures
```

Native UI fixtures cover view ordering, metric selection, saved configuration,
recipe reuse, filtering, raw data, cancellation, reconnection and compact layouts,
in addition to the existing shared-control and reduced-motion checks.

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
| `loss` | Logged training loss; solid orange line on the Loss graph |
| `eval_loss` | Held-out evaluation loss; dashed green line on the Loss graph |
| `learning_rate` | Optimiser learning rate; separate graph |
| `grad_norm` | Gradient norm when supplied by the trainer; separate graph |

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
keep the active run tracked and retry. A missing job (404) is surfaced as an
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

First layout iteration verification (2026-11-08, Linux): CMake build, theme audit,
and full UI suites at normal and 2× scale passed with zero failures. Added checks
cover shared-axis comparison samples, saved configuration differences, missing
validation, distinct-run selection, selection preservation after refresh, and the
unavailable full fine-tuning method. Populated dark/light previews were inspected
under `build/training-iteration1-ui/`; logs use `build/training-iteration1-*.log`.
These are synthetic UI fixtures, not new ML/hardware verification.

Second layout iteration verification (2026-11-08, Linux): CMake build, theme audit,
and full normal/2× UI suites passed with zero failures. Additional interaction
checks cover expanding the archive without replacing the live canvas, locking
the recipe during training, all three metric lanes, and compact settings without
horizontal overflow. Populated dark/light captures were inspected under
`build/training-iteration2-ui/`. Logs use `build/training-iteration2-*.log`.



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

## Integration with the remote feature branch

The local training branch incorporates `origin/feature` at
`76e44dd5549a52d1ee6dc1733e010984877eee18`. This adds the context-window meter and
auto-compaction, editable/retryable chat versions, saved chat history and
personalized greetings, and the collapsible sidebar. Training visualisation,
resource estimates and contextual run review remain in place.

Conflict resolution retains both the training and chat styles/test fixtures,
both sidebar and run-review navigation checks, and the current `dev`/`master`
branch/path filters for automatic verification. Saved chat defaults now follow
the backend's Windows `%APPDATA%/Magnite` convention; environment and Settings
data-directory overrides retain precedence. Incoming chat-focus tests use actual
Qt focus transfers so they can verify both entering an action and leaving a row.

The integration is local to `feature/training-visualisation`; neither `dev` nor
`master` is modified and the branch is not pushed.

Integration verification: CMake build passed; backend suite **248 tests**
(**247 passed**, **1 platform skip**); UI suites at normal and 2× scaling both
reported **0 failures**; theme audit and desktop selftest reported **0 failures**;
`actionlint` passed for the retained verification workflow. Combined UI captures
and logs are under ignored `build/feature-integration-*`. Native Windows/macOS
execution and GPU paths were not exercised by these local Linux checks.


## Training UX polish

The approved Studio/Observatory hierarchy and palette remain intact. The polish
pass adds a visible run-actions menu, dismissible feedback above the monitor,
keyboard-driven history selection, synchronized search in both views, explicit
empty/no-match states and a direct return from history to the active experiment.
Polling preserves history scroll positions and cannot overwrite modal validation
feedback. Pending cancellation stays disabled until the backend reports a
terminal state; rejected submissions restore the previous recorded snapshot.
Terminal runs show their actual duration, and absent validation is not presented
as a measured event.

The run editor keeps its header, validation feedback and actions visible around
scrollable fields. It includes model-folder/dataset-import actions, native
precision and target-module dropdowns, input focus/buddies, and Ctrl+Enter to
start. Run reuse opens a cancellable draft. Resource estimates debounce against
that draft and discard replies from older edits or closed dialogs. Saving and
starting are the only actions that apply the draft to the next-run recipe.

Focused regression captures, including empty and error states, are saved locally
under `design/training-polish-captures/`. Checks exercise draft isolation,
precision-aware estimates, validation, keyboard selection, cancellation races,
submission recovery, both themes and compact layouts. Full UI checks at 1x/2x
and the desktop smoke test supplement these targeted checks.

Raw JSON keeps the user's text selection and scroll position across live updates.
Missing or duplicated comparison selections clear the primary chart's old baseline.
Theme changes retint the run-card icons and legend. Feedback can be dismissed, and
recovering a backend connection does not clear a newer action's message.


Graph hover now uses an immediate, compact dark readout within the chart rather
than the platform tooltip. It shows the nearest raw values for each visible
series and identifies validation steps when they differ. Scientific axis labels
reserve enough space for their full notation.

Page scrollbars use a 3px orange handle near the workspace edge, with content
padding kept inside the scroll area. Training's inner lists, tables, raw data,
event log and recipe editor reserve a small gutter. Their handles appear only
during a scroll gesture and disappear after 850ms of inactivity; hover alone
does not reveal them. Nested boxes consume wheel gestures at either end instead
of moving the page. Unfocused recipe inputs do not change accidentally while
scrolling. Native focused controls and popup dropdowns keep their input behavior.

Observatory reserves 240px for the experiment index (200px in compact layouts)
and moves the right rail below the chart sooner. Run status and action controls
share a vertical center, with room for the ellipsis glyph.

The event log follows the selected experiment, reconstructs measured sample
entries, and deduplicates polling updates. It has timestamp/level styling,
message or severity filtering, copying of visible entries and Follow latest.
Scrolling up pauses following and updates preserve selection and scroll position.
Unknown historical timestamps remain visibly unknown. Each run retains at most
2,000 events. Focused screenshots are in `design/training-details-captures/`.

Detail-refinement verification: warning-free native build; focused training
checks and full UI suites at 1x/2x, theme audit, and an isolated desktop smoke
test all passed with zero failures. The smoke test used a temporary
`MAGNITE_HOME` so the open application could keep its data-directory lock.

Compare controls and baseline overlays are confined to Observatory. Returning
to Studio closes the comparison and restores the selected run's measured series.
Interactive focus, hover and checked states use backgrounds rather than accent
border highlights; keyboard input and selection remain available. Theme changes
apply palette and stylesheet with window updates batched, skip redundant mode
assignments, and coalesce Training's icon/log appearance refresh into one queued
update. Focused screenshots for this refinement are in
`design/training-scroll-captures/`, including active and idle inner scrollbars.

Scroll/appearance verification: the warning-free native build, focused checks
at 1x/2x, full UI suites at 1x/2x, theme audit and isolated desktop smoke test
passed. Regression checks cover transient handle visibility, quiet hover,
scroll containment at both ends, page-edge spacing, comparison visibility and
rapid mode switching with updates restored.
