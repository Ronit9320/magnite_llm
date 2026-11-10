# Docs reference panel

Docs is the sixth native workspace. It follows the approved edition 04 reference-library mockup: subject navigation, the data-focused overview, connected standalone articles, and questions that open relevant sections. It has no course progress, completion controls, or required article order.

## Content and distribution

`app/resources/docs/catalog.json` contains sixteen original articles, their sections, related articles, source links and three downloadable dataset examples. Five subjects cover models and learning, understanding data, training and evaluation, adaptation and inference, and Magnite reference. Small diagrams are painted by Qt; text uses `QTextBrowser`, with no browser engine, JavaScript runtime, backend request or background download.

Six Markdown references are bundled under `app/resources/docs/reference/`: `WORKFLOWS`, `LOCAL_API`, `INSTALL`, `LIMITATIONS`, `STORAGE_AND_PRIVACY` and `TROUBLESHOOTING`. These are snapshots of the corresponding repository documents, not runtime filesystem lookups. Official ML sources open in the user's browser only when clicked. Bundled Markdown opens in a matching opaque, rounded app-owned dialog. Linked documents outside the bundle are identified as unavailable locally.

The complete resource payload is less than 100 KB before Qt resource compression. It adds no videos, screenshots, model files or documentation framework. Packaged builds get the same offline library through `resources.qrc`; the localhost design preview is not needed.

Keep explanations grounded in the backend contract and current app behavior. When behavior changes, update affected sections and refresh the bundled Markdown from `docs/` in the same change. Review external reference URLs and the catalog's `reviewed` date. Do not copy entire external documentation pages into the bundle. The preview and native library are intentionally separate; native content changes go into the catalog.

## Reading tools

- Search ranks titles and matching sections locally. `/` or the platform's Ctrl/Command+K focuses search while Docs is active.
- The app's Ctrl/Command+6 opens Docs; Settings moves to Ctrl/Command+7. Ctrl/Command+, still opens Settings. Alt+Left/Right browses Docs history.
- Saved articles, collapsed subjects and quiet reading positions are stored under `docs/` in the existing local `QSettings` preferences. They do not change training data or model files.
- The reader uses the standard app text size; there is no separate Aa control. A future release will add font-size settings.
- The reader has a fixed control bar, active section links, related reading, sources and focus mode. Escape exits focus mode or closes the compact topic drawer. Focus mode restores the app sidebar when leaving Docs without changing its stored collapsed preference.
- Dataset examples offer prompt/response JSONL, chat JSONL and CSV, copy, wrap and atomic UTF-8 download. Arrow keys switch example tabs. These are illustrative records, not the user's imported data.
- Section links use `magnite-docs://article/<id>?section=<section>`. Copy a section link and paste it into Docs search, then press Enter. This is an internal link format, not an OS-registered URL scheme.

## Verification

```bash
cmake -S app -B build
cmake --build build -j
QT_QPA_PLATFORM=offscreen MAGNITE_DOCS_DESIGN_CHECK=1 ./build/magnite-app --ui-check /tmp/docs-captures
QT_QPA_PLATFORM=offscreen ./build/magnite-app --theme-check
QT_QPA_PLATFORM=offscreen ./build/magnite-app --ui-check
QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=2 ./build/magnite-app --ui-check
./build/magnite-app --selftest
```

The focused Docs checks run before any loopback fixture is started, exercising the library without a backend. They cover all articles and section links, search, dataset examples, saved articles, reading restoration, focus mode, compact navigation and source behavior. Captures include both themes and compact layouts. Full UI verification also includes these checks; existing desktop selftests exercise six workspaces plus the Settings window. `--screenshot` now captures Docs as `panel-5.png` and Settings as `panel-6.png`.
