# Magnite · Midnight panels

Six editable SVG screen concepts at **1440 × 960**: Models, Training, RAG, Adapters, Chat, and Settings. All use dark blue surfaces, blue accents, white stars, Space Grotesk headings, DM Sans body text, and the repository’s line icons.

## Review

Open `preview.html` in a browser. The top navigation switches screens. Enable **Preview star drift** to see slow background movement. The preview works offline and respects the system’s reduced-motion preference. Screen controls inside the SVGs are illustrations, not a working application.

`previews/` contains PNG renders for quick review; `overview.png` shows all six screens. Model names, file names, sizes, statuses, and training metrics are illustrative.

## Import into Figma

1. Extract `magnite-midnight.zip` if using the package.
2. Open the existing file: https://www.figma.com/design/xUHozM4MMAHael075MnTFC
3. Open or create a page called **Desktop Panels**.
4. Drag the six files from `panels/` onto the canvas. Import the SVGs, not the PNG previews.
5. Arrange them in two columns with Models / Training, RAG / Adapters, and Chat / Settings.

The SVGs contain vector shapes, groups, and text, with no embedded screenshots or external image dependencies. They do not carry Figma auto-layout, native component bindings, or prototype links. Figma may outline SVG text during import; `generate.py` and the SVG source retain all text for editing and regeneration. The bundled fonts and their OFL licenses are in `fonts/`.

## Motion handoff

The imported SVGs are static. The named **Starfield** group sits behind the interface. Animate it from `(0, 0)` to `(8, -12)` over 24 seconds with ease-in-out, then reverse; the full loop takes 48 seconds. Keep cards, navigation, and text still. Reduced motion freezes the stars. The browser preview demonstrates this motion; it does not add animation to Figma or the Qt app.

## Scope and implementation notes

- This is the approved blue-and-white design direction; other themes are deferred.
- Appearance controls for floating stars and reduced motion are proposed UI additions.
- Panels prioritize current workflows; advanced training parameters are shown collapsed. The current application remains unchanged.
- Status uses explicit labels and icons instead of green, amber, or red. Destructive actions require confirmation when implemented.
- The Chat and Adapters screens illustrate different sessions: Chat uses llama.cpp without an adapter; Adapters shows an active compatible adapter session.
- Dense content sits on opaque blue surfaces so stars cannot reduce text readability.

## Regenerate

From the repository root, run `python design/midnight/generate.py` (Python standard library only). This writes the six SVGs, `tokens.json`, bundled fonts, and `preview.html`. PNG rendering additionally needs `rsvg-convert` with the bundled fonts available to fontconfig. The source palette is centralized in `generate.py`; `tokens.json` is its generated export.

All six SVGs were parsed and rendered locally and their PNGs visually inspected. Figma import has not been tested because the connected Starter plan exhausted its MCP allowance.
