# Studio 03 — Instrument console

A native desktop workspace on `UI-studio-3`. Cream/orange light and charcoal/orange dark surfaces, editorial headings and compact native controls. Light mode remains available from the session header.

- Vertical tool rail with Ctrl+1–6 workspace shortcuts.
- Compact session header with backend status and a persistent theme switch.
- Model rack with live workspace modules, repository search and operational empty states.
- Drag the divider in Inference to resize the conversation and parameter inspector.
- Existing training, knowledge, adapters and model-loading actions remain connected to the backend.
- Reduced-motion and grid preferences use isolated `studio3` settings.

Colors live in `app/src/theme.h`; `console.qss` is loaded under the existing theme resource alias.

Build: `cmake -S app -B build && cmake --build build -j4`. Launch: `./build/magnite-app`. Verification: `--theme-check`, `--selftest`, and `--screenshot <dir>`.

Panel revision: Training has a narrow run setup inspector and tabbed metrics/logs. Knowledge pairs a document collection with a query laboratory. Adapters uses full-width registry rows and a persistent import toolbar. Inference embeds its composer in the conversation stream beside left-hand channel controls. Settings groups controls into Runtime, Storage & privacy, and Appearance tabs.

## Combined studio refinement

User requested the best of Studio 2 and 3: restored Studio 2 cream/orange and charcoal/orange palettes, editorial Space Grotesk headings and a restrained animated seal with a single-line studio note in the library. Retain Studio 3 vertical navigation, compact controls, split inspectors, telemetry tabs, adapter registry and category settings. The existing theme and reduced-motion switches apply to the combined design.

## Production refinement

- A consolidated token-based stylesheet gives controls consistent focus, hover, disabled and error states in both modes.
- Overview shortcuts are native buttons: Tab to focus and Space to activate, with accessible names and state descriptions.
- Library and conversation empty states explain the next action; clearing a conversation restores its guidance.
- Compact status pills, quieter resize dividers and corrected adapter row padding keep attention on workspace content.
- Built-in selftests cover theme persistence, keyboard overview navigation and conversation empty-state restoration.

## Modern conversation workspace

Centered conversation column with prompt starters, multiline input, Enter to send and Shift+Enter for newlines. Model selection stays visible; engine and sampling controls expand from Model settings. New chat stops any active generation before clearing the session. Dark mode uses neutral graphite surfaces and orange accents. This refinement selects dark mode once on first launch; subsequent theme selections persist.

Magnite-specific chat identity: “Your model. Your way.” with the studio seal and three functional workspace actions: test the model, ask ingested documents (enables Knowledge), and shape the response (opens model settings). Composer copy refers to the user's model; no generic assistant prompts or unconditional privacy claims.

Header polish: replace the backend-style badge with a quiet dot and Connecting / Connected / Unavailable text. Connection details live in a tooltip; no extra bottom status bar appears on connection. Separate the understated theme switch with a fine divider.

## Contributor rules for the local release

This is the active design; earlier design folders are historical references.
Use theme tokens in `app/src/theme.h` and the active QSS resource. Never add hex
colors directly to widget code or QSS; extend Theme::selfCheck with new tokens.
SVG icons use `stroke="currentColor"` and Theme::coloredIcon for tinting.
Preserve light/dark themes, readable labels, focus states, keyboard operation,
and reduced motion. Verify changes with `--theme-check`, the bare selftest,
and screenshots of affected panels in both themes. New feature styling should
follow existing components; propose unestablished patterns before redesigning.
The public release covers local training, inference, adapters, and RAG only.
