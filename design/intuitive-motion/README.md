# Magnite — Studio Motion

UI exploration on `design/intuitive-motion`, based on master’s Studio 03. This
branch implements the user-requested redesign; Studio 03 remains the starting
point for its paper, graphite and orange palette and native workspace navigation.

- A wider navigation rail gives labels room and introduces a persistent seal.
- The existing radial logo gains two registration rings, 48 fine markings,
  twelve rounded rays and a faceted core. Qt paints the mark at any size using
  the current theme; there is no bitmap scaling or additional asset dependency.
- Rounded surfaces, larger control targets and stronger headings carry across
  Library, Training, Knowledge, Adapters, Playground and Settings.
- Training opens with the run monitor and metrics. A scrollable setup inspector
  keeps hyperparameters reachable in smaller windows. Preparing and Training
  button labels follow actual request/job state; the seal rotates with an orbit
  while a job is submitted and runs. Completion and failure stop its motion.
- The conversation composer highlights on focus. A compact animated seal and
  Responding label accompany generation. Completion, interruption, clearing
  and request failures restore the send action and hide the activity indicator.
- Mouse and keyboard button activation get a 320 ms ripple while native Qt
  actions and focus remain intact. Existing panel fades remain in place.

Light/dark appearance and the existing Reduce motion preference remain available.
Reduced motion freezes activity marks, suppresses ripples and blinking response
cursors, and skips transitions. Animation timers stop when marks are hidden or
the application is inactive. The sidebar mark stays static; ambient welcome
marks retain their slow rotation. Idle training marks stay static.

All colors come from `app/src/theme.h`; the stylesheet continues to use the
existing theme resource alias. No backend or inference engine changes.

## Review

Build and launch using the repository’s standard commands:

```bash
cmake -S app -B build
cmake --build build -j4
./build/magnite-app
```

Static review captures in `previews/` show every workspace in both modes.
Verification includes the theme audit, bare desktop selftest (including chat
error recovery and activity reset), and a temporary Qt runtime probe that
compares activity frames, checks static reduced-motion frames, and exercises
mouse/keyboard clicks and ripple cleanup. Real-model training and generation
were not run for this design pass.
