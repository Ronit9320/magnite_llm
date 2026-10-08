# Magnite logo preview

The supplied PNG and SVG are the source presentation board. The application
uses transparent vector extracts in `app/resources/brand`: the standalone symbol
and the outlined wordmark, without the board background or tagline. The soft SVG
blur is omitted for Qt SVG compatibility. Geometry comes from the supplied SVG.
Brand colors are tokens in `theme.h`; the wordmark changes to light text in dark
mode while its orange i and the symbol retain the supplied colors.

The symbol replaces the previous wheel in the sidebar, window icon, Library,
conversation welcome, and training/chat activity indicators. Existing animation
and reduced-motion behavior apply. The sidebar uses the supplied wordmark.
This user-requested identity supersedes the wheel artwork in Studio Motion.

The supplied `Magnite _ Animated Icon.mp4` now supplies the looping motion in
the sidebar, Library, conversation welcome and active training/chat indicators.
`app/resources/brand/animated-symbol.gif` is a 256px, 25fps derivative with the
black video background keyed transparent. Qt's existing QMovie support plays it
without a multimedia runtime dependency. Hidden/inactive widgets pause playback;
reduced motion and idle activity indicators use the static vector symbol.
The window icon and wordmark remain vector artwork.

Preview captures are in `previews/dark` and `previews/light` (panel 0 is Library,
panel 4 is Playground). Regenerate with the app's `--screenshot` option.
