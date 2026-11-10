# Settings window

Settings is a native modal utility window above the current workspace, opened by
the sidebar, Ctrl+6 or the platform Preferences shortcut (Ctrl/Command+,). Closing
it preserves the workspace and contextual run page underneath. Escape and the
close button dismiss it. The eight categories follow the reviewed browser mockup;
the app does not embed that prototype or require its server.

## Preferences and actions

- **General:** local greeting name, shortcut reference, confirmed preference reset.
  Reset affects the preferences shown in Settings, except the name/data directory.
  It preserves saved data, per-model/per-panel recipes and updater trust state.
- **Appearance:** Light/Dark/System, instrument grid, Reduce motion and three text
  sizes. Text sizing uses the shared QSS, including open dialogs. System follows
  desktop scheme changes on Qt 6.5+; older Qt uses the scheme detected at startup.
- **Chat:** Enter versus Ctrl/Command+Enter to send, context-meter style and
  auto-compaction threshold. Shift+Enter always inserts a newline. Generation
  parameters stay in Playground.
- **Runtime:** default local inference engine; applies to the next model load.
- **Storage & privacy:** active data directory, next-start path preference, bounded
  workspace/shared-cache measurements, free space and folder opening. Changing
  app home does not move files. MAGNITE_HOME wins over preferences. Model deletion
  stays in Library. A lower-bound label marks partial measurements; symbolic links
  are excluded and nested locations are counted once.
- **Chat data actions:** atomic JSON export with complete branch trees and metadata;
  confirmed deletion of saved chats, disabled while chat is busy. An unreadable
  listed chat fails the export. Cancelling the export never changes saved data.
  Export is not an import/restore or whole-workspace backup feature.
- **Notifications:** training/download completion or failure, optional sound, and
  a test action. Normal alerts are sent only while the app is in the background;
  cancelled work does not trigger a failure alert. Linux uses the desktop D-Bus
  notification service when Qt DBus is available, with system-tray fallback. Other
  platforms use Qt's tray integration. Desktop permissions, Do Not Disturb and
  sound policies can suppress alerts. No prompts/documents/model names are sent.
- **Diagnostics:** real backend/package versions and device telemetry without
  importing ML runtimes. Installed packages are not a compatibility guarantee.
  Review/save reports select fields that omit local paths, model IDs, chat content,
  credentials and logs. The separate read-only local log viewer supports filtering
  and copying; logs can include personal paths and model names. Reads are bounded
  and redact common credential patterns. Nothing is uploaded.
- **Updates:** existing signed update check/download/cancel/install behavior,
  startup checks and a link to release notes. Existing backend quiescing blocks
  installation during training, pulls and active HTTP/SSE work. In-app installation
  still depends on package/platform support; this change does not expand updater
  platform coverage.

Immediate preferences persist through QSettings; runtime/data-directory changes
use Save changes. New read-only backend routes are documented in
[Local API](../LOCAL_API.md). Existing persistent data formats and release identity
are unchanged.

## Motion and verification

The window fades over 140 ms; category content uses a short opacity transition.
Buttons, switches and dropdowns retain the shared native animation/focus behavior.
Reduce motion snaps category transitions and disables window fades. Native window
opacity depends on the compositor; offscreen checks skip window opacity animation.

Focused deterministic captures and interaction checks need no model downloads:

```bash
QT_QPA_PLATFORM=offscreen MAGNITE_SETTINGS_DESIGN_CHECK=1 ./build/magnite-app --ui-check /tmp/magnite-settings-captures
QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=2 ./build/magnite-app --ui-check /tmp/magnite-ui-2x
QT_QPA_PLATFORM=offscreen ./build/magnite-app --theme-check
PYTHONPATH=backend/src .venv/bin/python -m unittest discover -s backend/tests -v
```

The focused checks cover all categories, both themes, layout fit, actual text-size
changes, reduced motion, send-key behavior, branch-preserving export, corrupt-file
export rejection, cancel/confirm deletion, reset scope and Escape/reopen behavior.
The full UI suite also captures compact settings categories and exercises the
existing workflows. OS notification presentation, file pickers and compositor
appearance require manual acceptance on the target desktop.

## Verification evidence (2026-11-10)

The native build, theme check, complete UI checks at 1×/2×, focused settings
captures and desktop/backend startup/navigation selftest passed on Linux. Backend
regressions include bounded scans, nested-cache accounting, credential redaction,
authenticated diagnostics and update-gate handling. No real models were downloaded
for these settings checks.

The changed settings/window/theme/MainWindow C++ sources also passed syntax checks
against Qt 6.2.4 headers. A full Qt 6.2 configure was blocked by the pre-existing
`compress="0"` animated-symbol resource entry; that unrelated resource is unchanged.
The executable and runtime checks above use the installed current Qt.

### Settings presentation refinement

The sidebar footer and Updates category pair the static supplied symbol with the
wordmark at its natural proportions. Shortcuts use a frameless settings-style
utility dialog and a keycap list; report/log dialogs share that frame. Storage is
shown as a measured total, separate cache/workspace metrics, a file-distribution
bar and a free-space value. The bar compares measured file sizes across locations,
not disk capacity; partial scans retain lower-bound labels. Diagnostics use
version/system/device cards and individual runtime-package status rows.

Focused settings checks at 1× and 2× capture these views and verify that the
shortcuts dialog requests the frameless window flag. The theme audit also passes.
