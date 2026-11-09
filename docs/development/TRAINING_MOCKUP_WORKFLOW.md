# How the training mockups were created

This records the browser exploration that led to the native Training Studio and
Observatory. It explains the design process, rather than defining a second UI
implementation to maintain. The current Qt code and theme tokens are the source
of truth; the browser preview predates several requested refinements.

## 1. Inspect the existing app

The starting point was the existing Qt training panel, its backend contract,
shared controls, and the app's light/dark theme. The exploration kept the orange
accent, warm light surfaces, dark surfaces, typography, and brand symbol.

The prototype used copies of the app's DM Sans and Space Grotesk font files and
brand symbol. Font license files accompanied the copied fonts. No raster artwork
or image-generation service was needed: the layout and charts were built in code.

## 2. Build a separate, disposable browser prototype

The working directory was `/tmp/magnite-training-mockups`, outside the official
repository. Plain HTML, CSS and JavaScript were sufficient; Astro and a frontend
build pipeline were not needed.

| File | Purpose |
| --- | --- |
| `index.html` | Preview shell, concept navigation, theme switch, toast and configuration dialog |
| `style.css` | Theme variables, local fonts, layouts, component styles and responsive rules |
| `app.js` | Illustrative runs, view state, SVG charts and simulated interactions |
| `assets/` | Copied fonts, their licenses and the brand symbol |
| `check.cjs` | Playwright interaction checks and responsive screenshots |
| `capture-reference.cjs` | Browser reference captures for native implementation |

The preview was served as static files, bound to loopback:

```bash
python3 -m http.server 4321 --bind 127.0.0.1 --directory /tmp/magnite-training-mockups
```

While that foreground command is running, the views are available at
`http://127.0.0.1:4321/#studio` and
`http://127.0.0.1:4321/#observatory`. Stop it with Ctrl+C when finished.
The native application does not depend on this server.

The temporary directory and browser screenshots are local artifacts. They are
not included in a fresh clone, and `/tmp` is not permanent storage. These commands
only reopen the original prototype while its files still exist. The tracked Qt
fixtures described below can regenerate screenshots of the implemented page.

## 3. Explore and refine the information layout

The exploration separated two tasks:

- **Studio:** follow the current run, see its configuration and local compute,
  then prepare the next experiment.
- **Observatory:** browse experiments, inspect several signals and compare runs.

An initial guided concept was removed after review. First-run help was left as
a separate future tutorial, and a single configuration dialog served new runs.

The visual structure used a quiet sidebar, a clear page heading, a main signal
card, a supporting right rail and an experiment table. Observatory added a
persistent experiment index on the left. Important measurements were large;
metadata, chart legends and configuration labels were smaller. Borders, spacing
and restrained surfaces separated groups without introducing a new color scheme.

CSS custom properties held the two palettes. CSS Grid and Flexbox controlled
the columns and card layouts, with media queries that stacked content at smaller
widths. Inline SVG paths supplied the charts, grid, validation series and hover
cursor. Small inline SVG icons kept the prototype independent of an icon package.

## 4. Make the preview interactive

JavaScript held separate state for the selected run, next-run recipe, visible
metric, theme, smoothing, comparison and event-log expansion. Rendering functions
built the two workspaces from that state; delegated event handlers connected the
buttons and inputs to those functions.

The preview supported selecting historical runs, editing the next recipe,
creating or duplicating a run, simulated start/stop, chart hover, smoothing,
comparison, log expansion, experiment search and JSON export. Run configuration
used an HTML dialog. Keeping the next recipe separate from the selected run made
it possible to check that editing future settings did not rewrite past results.

All browser measurements, device readings and progress were illustrative. The
preview had no backend connection and did not start training. Its simulated
curves and timing were never production telemetry.

## 5. Check and capture the browser design

Playwright was used as a development tool, not a runtime dependency. The local
`check.cjs` exercised both concepts in both themes, historical selection,
recipe editing, duplicate/new/stop, cancel, smoothing, comparison, logs, search
and export. It checked horizontal overflow at 1280, 1024, 768 and 390px widths
and collected screenshots and browser errors.

`capture-reference.cjs` used a 1440px browser viewport and waited for local fonts
to finish loading. It captured full pages, the workspace alone and the new-run
dialog in both themes. Cropping the workspace removed the design-explorer
navigation so native comparisons measured the actual product area.

The original scripts use a machine-specific Chromium executable and output paths.
If reusing them elsewhere, update those paths or use a locally installed
Playwright browser. With the original files and browser available:

```bash
cd /tmp/magnite-training-mockups
node check.cjs
node capture-reference.cjs
```

Reference images were saved to `design/training-preview-reference/`. Later
native captures were saved under `design/training-native-captures/`,
`design/training-polish-captures/`, `design/training-details-captures/` and
`design/training-scroll-captures/`. These directories follow the repository's
existing local-only design ignore rule.

## 6. Translate the approved views into native Qt

The layout was implemented with Qt widgets and layouts in
`app/src/panels/trainingpanel.cpp`, using `TrainingChart` for measured charts.
The existing animated dropdowns and shared native controls were retained.
Styles live in `app/resources/theme/console.qss`; production colors come from
`app/src/theme.h` and the runtime token substitution in `app/src/theme.cpp`.
No browser engine or prototype JavaScript was embedded in the desktop client.

The native implementation deliberately incorporated the later review decisions:

- The live Studio monitor comes before the next-run recipe.
- Compare controls and baseline overlays belong to Observatory.
- Graph hover uses an immediate, compact in-chart readout of raw measurements.
- Observatory's experiment index is wider than the original browser reference.
- Page scrollbars are 3px wide and sit near the outer edge. Inner handles appear
  during scrolling, hide when idle and keep gestures contained at their limits.
- Interactive controls use background changes instead of border highlights.
- The event log follows the selected experiment and adds filtering, copy,
  severity styling and a pauseable Follow latest control.

Production data comes from actual job snapshots and device telemetry. Missing
measurements remain unknown, throughput is labelled steps/second, and device-wide
readings are not attributed to a selected historical run. These distinctions are
covered in [Training visualisation](TRAINING_VISUALISATION.md).

## 7. Verify the implementation without the web server

The tracked native UI fixtures supply deterministic measurements and require no
model downloads. From the repository root, after building the app:

```bash
QT_QPA_PLATFORM=offscreen MAGNITE_TRAINING_DESIGN_CHECK=1 ./build/magnite-app --ui-check design/training-scroll-captures
QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=2 MAGNITE_TRAINING_DESIGN_CHECK=1 ./build/magnite-app --ui-check /tmp/magnite-training-captures-2x
QT_QPA_PLATFORM=offscreen ./build/magnite-app --theme-check
```

The focused checks cover both themes, compact layouts, chart hover, comparison
visibility, scroll containment and transient handles, log controls, recipe
isolation, validation and submission recovery. Full UI checks at 1x/2x and the
desktop smoke test supplement the focused captures.
