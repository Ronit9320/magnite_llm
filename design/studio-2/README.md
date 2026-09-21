# Studio 02 — Independent thinking

Branch: `UI-studio-2`. A bright editorial workbench for local intelligence.

- Warm paper canvas, near-black ink, safety orange actions and violet focus rings.
- Horizontal numbered navigation places the five workspaces beneath the masthead.
- Oversized typography and a rotating twelve-spoke graphic anchor Discover.
- Fine registration marks, crisp borders and compact rectangular controls replace soft glass surfaces.
- All six panels share the same controls, status treatments and typography.
- Appearance settings independently persist the grid and reduced-motion choices for this branch. Motion stops while the app is inactive or its animated panel is hidden.

Colors remain centralized in `app/src/theme.h`. The resource alias `theme/theme.qss` loads `app/resources/theme/workbench.qss`. Existing model, training, retrieval, adapter and chat operations retain their backend contracts.

Build with `cmake -S app -B build` and `cmake --build build -j4`; run `./build/magnite-app`. Audit with `./build/magnite-app --theme-check`, check navigation with `--selftest`, and capture the six panels with `--screenshot <dir>`.
