# Magnite Studio

An alternative desktop direction on branch `UI-studio`, requested on 22 September 2026. The existing `UI` branch retains the Midnight design.

## Visual direction

Graphite-green canvas, warm cream typography, pale mint primary actions with dark text, and lavender secondary accents. An inset navigation rail and rounded opaque cards create a quieter, more tactile workspace. Existing Space Grotesk and DM Sans fonts remain bundled and work offline.

The Model library opens with an editorial introduction, executable search suggestions, and shortcuts into Training, Knowledge, and Playground. These use real application actions; no sample models or metrics are inserted into the backend.

## Motion

A subtle mint/lavender glow moves on a 32-second cycle over a stationary dot grid. Panel changes fade in over 220 ms. Animation pauses when the application is inactive or hidden. Settings provides an Ambient canvas switch and Reduce motion option, with Studio-specific persisted preferences.

## Source of truth

Palette: `app/src/theme.h`. Styling: `app/resources/theme/theme.qss`. Ambient rendering: `app/src/widgets/starfield.cpp` (the existing class name is retained to avoid unnecessary churn). Tokens are substituted longest-first and the built-in audit checks generated color literals.

The user explicitly authorized a new palette and UI style, superseding the old blue/white and static-background requirements for this branch only. No external fonts or image services are required.

## Build and review

```sh
cmake -S app -B build
cmake --build build -j
./build/magnite-app
./build/magnite-app --theme-check
```

The executable must remain at `build/magnite-app` so the existing backend launcher resolves the repository correctly.
