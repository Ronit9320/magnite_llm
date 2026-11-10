# Shared native UI components

This refinement uses shadcn/ui's component hierarchy and visual states as design
references while keeping Magnite's C++/Qt Widgets architecture and its existing
paper, graphite and orange theme. These are native controls, not embedded web
components. No JavaScript dependency or external component source is bundled.

References reviewed: [Button](https://ui.shadcn.com/docs/components/radix/button),
[Input](https://ui.shadcn.com/docs/components/radix/input),
[Tabs](https://ui.shadcn.com/docs/components/radix/tabs),
[Select](https://ui.shadcn.com/docs/components/radix/select),
[Dropdown Menu](https://ui.shadcn.com/docs/components/radix/dropdown-menu), and
[Alert Dialog](https://ui.shadcn.com/docs/components/radix/alert-dialog).

## Component contracts

| Control | Implementation | Behavior |
| --- | --- | --- |
| Action button | `widgets/button.h` | Quiet outline/primary/destructive/ghost variants; eased hover color, pressed feedback and a keyboard focus ring. Existing object names select variants. |
| Boolean setting | `widgets/toggleswitch.h` | The existing animated track/thumb switch; native checked state, shortcuts and accessibility. |
| Select | `widgets/combobox.h` | Muted popup selection with a checked current item and animated chevron; native editable text, keyboard navigation, Escape and selection semantics. |
| Text/number fields | Native editors plus `widgets/componentfocus.h` | A short focus-halo transition that cannot take focus or intercept input. Embedded spin/select editors share the outer control's halo. |
| Number stepper | `widgets/spinbox.h` | Native ranges, text editing and stepping with quiet stepper backgrounds and theme-aware arrows. |
| Tabs | `widgets/tabwidget.h` | A single segmented track with a sliding selected pill. Native pages and selection change immediately; keyboard and disabled-tab behavior remain native. |
| Progress | `widgets/progressbar.h` | Rounded, interpolated presentation; the actual numeric value and signals update immediately. New-operation resets snap to zero. Indeterminate/vertical bars retain native painting. |
| Context menus and tooltips | Shared QSS | Themed surfaces, padding, separators, disabled states and quiet selection. Native focus, Escape and menu navigation remain in use. |
| Window frames | `widgets/windowchrome.h` | Compact app-owned controls replace the system title bar. The main window reuses its workspace masthead; dialogs and Qt file pickers receive a small title/close row. Settings keeps its existing controls. Drag the header to move, drag window edges to resize, or double-click the main masthead to maximize/restore. |
| Removal confirmation | `widgets/confirmation.h` | Spaced dialog content and shared action buttons. Cancel remains the default and Escape action. |

Use the shared classes when creating ordinary controls. Specialized sidebar,
overview and chart widgets retain their dedicated implementations. All colors
come from `Theme`; components do not introduce widget-local hex values.

## Motion and responsiveness

`widgets/motion.h` centralizes bounded scalar transitions. Theme motion durations
are 140 ms for small feedback and 180 ms for larger transitions. Page navigation
now uses a brief 180 ms fade instead of the previous 600 ms fade. Hover changes
retarget from the currently rendered value rather than queueing animations.
Components stop or settle animations when hidden; unchanged values do not start
new timers. Existing switches and charts keep their own appropriate timings.

**Reduce motion** applies to these transitions as well as the existing app
animations. Control state, emitted signals and data never wait for animation.
The global click effect skips shared animated buttons, preventing a second
ripple from competing with their hover/press treatment.

The focus layer is a transparent child with no focus policy. It follows input
resizes and checks whether focus remains in the control before fading out. It
is only created when a field is focused. No field is replaced with a fake input.

## Verification

```bash
cmake -S app -B build
cmake --build build -j2
QT_QPA_PLATFORM=offscreen ./build/magnite-app --window-check build/window-controls
QT_QPA_PLATFORM=offscreen ./build/magnite-app --theme-check
QT_QPA_PLATFORM=offscreen ./build/magnite-app --ui-check build/component-polish-ui
QT_QPA_PLATFORM=offscreen QT_SCALE_FACTOR=2 ./build/magnite-app --ui-check
QT_QPA_PLATFORM=offscreen ./build/magnite-app --selftest
```

The UI check includes a small component gallery, dark/light captures, button
activation/disabled behavior, transition settling, tab keyboard navigation,
progress state versus presentation, focus-layer input transparency and menu
keyboard dismissal. Existing panel checks cover compact layouts, select
keyboard/Escape handling, switch accessibility, chat workflows and training
visualisation. Captures and logs are generated under ignored `build/`.

Verified locally on Linux: CMake build passed, theme audit reported **0
failures**, full UI checks at normal and 2× scaling reported **0 failures**, and
the desktop startup/navigation selftest reported **0 failures**. The normal-scale
run also generated the component gallery, select/menu and populated panel
previews. Native Windows/macOS appearance requires checking on those platforms.

## Dark palette and text containment

Dark mode now uses near-black canvas and inset layers, with separate darker card,
hover and pressed surfaces. Light-mode colors are unchanged. The theme audit
checks dark elevation order and text contrast in both appearances.

Shared buttons lay text out inside the native padded content box. Single-line
labels elide when constrained while retaining their complete accessible name;
truncated labels without an existing tooltip expose the full text on hover.
Multiline action labels wrap and provide height-for-width sizing. Icons and
labels are laid out together inside the same clipped content area.

Tab labels are centered and clipped within their own padded tab bounds, rather
than delegating label positioning with a mismatched stylesheet rectangle. UI
regressions compare rendered text against its content bounds and cover narrow
multiline actions, accessible labels and tab ink containment at normal/2× scale.
Updated captures are under `build/darker-layout-ui/`.

The window-control checks cover both themes, normal and 2× scaling, safe dialog
cancellation, repeated opening, header resizing, maximize/restore, and resize
cursors. Native Wayland checks use the same test command without the offscreen
platform override. Minimized state is checked offscreen because
[Qt's Wayland integration](https://github.com/qt/qtwayland/blob/6.8/src/plugins/shellintegration/xdg-shell/qwaylandxdgshell.cpp)
does not receive that state from the compositor. Native Windows/macOS frame
behavior still requires acceptance on those platforms.
