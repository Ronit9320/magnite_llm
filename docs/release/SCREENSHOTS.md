# Current interface screenshots

Capture the finalized interface from the current build:

```bash
cmake -S app -B build
cmake --build build -j2
QT_QPA_PLATFORM=offscreen ./build/magnite-app --screenshot /tmp/magnite-screenshots
```

Use disposable data/settings as described in [CONTRIBUTING.md](../../CONTRIBUTING.md).
The command captures all six panels and exits. For populated synthetic previews
in both themes, run:

```bash
QT_QPA_PLATFORM=offscreen ./build/magnite-app --ui-check /tmp/magnite-ui-previews
```

Quick captures may show Connecting while the backend starts. Fixture previews
use synthetic data; neither is end-to-end model verification. For a real populated
workflow recording, follow the [demo runbook](DEMO.md).

Retired design mockups and earlier-identity screenshots are no longer published
as current previews. Keep generated captures outside the source tree.
