# Blueprints editor redesign

Branch: `feature/blueprints-editor-redesign` (from `dev` at 42629c9).
Reference mockup: `docs/development/blueprints/mockups/editor-v2.html` (open it in a browser).

## Why

The owner finds the current Blueprints editor doesn't "feel like it": it should be
closer in spirit to Unreal Engine's Blueprint editor, more modern and polished,
with borders removed so the graph is easier to read. The node selector in
particular has to change. Today it is a modal dialog of 3×3 card pages, and
nodes are connected by clicking ports or through a **Connect** dialog instead of
by dragging wires.

The owner reviewed and refined an interactive HTML mockup and asked for it to be
built into the app, with the canvas rebuilt on `QGraphicsView`.

## What the user sees

Opening a blueprint shows a full-bleed dotted canvas with everything else
floating over it. There are no borders; panels separate from the canvas by shade
and soft shadow.

- **Top bar:** a back arrow, the blueprint name with its revision and save state,
  and a Draft/Revision pill. A segmented control **Build · Test cases · Run
  history** replaces today's tabs (Canvas · Test & run · Run history). On the
  right are a live validation status ("Ready to run", or "2 inputs not
  connected"), and **Run**. Publish, Reload and Open in Conversation keep their
  current conditions and move into this bar. A ⋯ menu holds the blueprint-level
  actions the bar has no room for: Edit details (where the description lives),
  Duplicate, Delete blueprint, and Back to Conversation when it applies. The
  "Fixture data" marker becomes a small pill beside the name in fixture mode.
  *(Builder's call, confirmed in review.)*
- **Nodes:** rounded cards with a header tinted by node family (sources,
  transform, knowledge, models, tools, output), an icon, the node name and its
  operation. Input pins sit on the left edge and output pins on the right, each
  labelled with its name and type. Pin and wire colours follow the data type
  (text, number/integer, boolean, object, array, receipt, adapter). Wires are
  curved and carry a faint glow. A node with an unconnected required input shows
  a warning mark. Some nodes show a short preview: the template text, the
  checkpoint, the run input's sample.
- **Connecting:** drag from an output pin to an input pin. While dragging, the
  inputs that accept the type are highlighted. Dropping on an incompatible pin
  does nothing and explains why, using the existing `connectionError` text.
  Dropping on an input that already has a wire replaces that wire, as in
  Unreal (remove, then add, through the existing paths). Dragging an existing
  wire off an input disconnects it, and you can drop it on another pin. Dropping
  a wire on empty canvas opens the quick-add menu, filtered to nodes with a
  compatible input, and the new node arrives connected.
  *(Replace-on-drop is the builder's call.)*
- **Ports whose type you choose** (`chooseType`: model inputs and outputs,
  template and build-object fields, choose, compare, constant) accept any type
  they can be set to. Connecting a wire to one sets that port to the wire's
  type, exactly as choosing it in settings would; a port change that would
  break other wires asks first, as `reconcilePorts` does today. They count as
  compatible for highlighting and for the filtered quick-add.
  *(Builder's call.)*
- **Removing wires:** click a wire to select it (it brightens) and press Delete,
  or use **Disconnect** on a wired row in the inspector. The Connect dialog
  stays in the node's right-click menu as the keyboard route for making and
  removing connections. *(Builder's call.)*
- **Node library dock (left):** search (`/` focuses it), category chips, and
  operations (not node kinds) grouped by family with a one-line description.
  Drag a node onto the canvas or click to add it at the centre. Nodes saved with
  "Save to node library…" appear as a **Saved** group (and chip) with their
  remove action, and also show in search and quick-add. A second tab, **In this
  workflow**, lists the graph's nodes with a "needs input" marker. It replaces
  today's node tree, and clicking a row selects and centres that node. The dock
  can be collapsed. *(Saved group: builder's call.)*
- **Adding is instant (owner's decision):** a node from the library or
  quick-add drops in at once, built with `BlueprintPreview::makeNode` defaults,
  and connects if it came from a dropped wire. No setup dialog opens. You finish
  it in the inspector. A node that still needs something to run, such as a
  model without a checkpoint, shows the warning mark, and the top bar counts it
  as an issue.
- **Quick-add menu:** opens at the cursor on Space or Tab, right-click, or a
  double-click on empty canvas, and also from **Add node** in the dock. It is a
  search field plus a grouped list driven by the keyboard (↑ ↓ Enter, Esc).
  This replaces the modal node picker.
- **Inspector (right), edited in place (owner's decision):** floats in when a
  node is selected and slides away when nothing is. Its fields edit the node
  directly: name, operation setting, checkpoint and adapter binding, response
  format, instructions, template, port names and types, dataset, and test
  value. Each change goes through `BlueprintPanel::changeNode` (on commit:
  focus-out or Enter for text, immediately for pickers), so validation and
  saving stay where they are. A change that alters ports uses the existing
  `reconcilePorts` confirmation before any wire is dropped. The modal Configure,
  binding and dataset dialogs leave the main flow; their logic is reused,
  not rewritten. Below the fields: the node's inputs and outputs with what
  each is wired to (with Disconnect), **Test this node** and **Delete**.
- **Details window:** an ⓘ on each node (shown on hover or selection), on
  library and quick-add entries, and in the inspector header opens a small
  movable window. It has tabs: **Overview** (what the node does, a before/after
  example, tips), **Inputs & outputs** (each pin explained, and on a canvas node
  what it is wired to) and **Last run** (status, time and output from the most
  recent run or node test). Shortcuts: `i` on a selected node, Shift+Enter in
  the quick-add menu. It opens beside what was clicked, in the free space
  between the library and the inspector.
- **Bottom dock:** library toggle, **Add node**, undo/redo, zoom −/%/+ and Fit
  (`F`).
- **Minimap** in the bottom-right corner showing the nodes and the visible area.
  Clicking or dragging in it moves the view.
- **Run runs on the canvas (owner's decision):** **Run** starts at once from
  the Build view. It uses the test case or sample input currently selected in
  Test cases (the first one if none is chosen yet), with the same rules as
  today's run setup: a draft runs as a draft test, and in live mode a full run
  still needs a saved revision, in which case Run offers to save one. While it
  runs, nodes show running, done (a small ✓) and failed states, and wires into
  the active node animate. When it ends, a toast shows the result with a link to
  the run in Run history. Each node's last output feeds the details window's
  **Last run** tab. A ▾ beside Run picks the input or opens Test cases for the
  full setup.

## Goals

- Ship the look and interactions of the mockup inside the Qt app, in both dark
  and light themes.
- Replace the modal node picker and the Connect dialog as the main way to build
  graphs.
- Draw everything from the operation catalog in `BlueprintPreview`, so nodes
  added on `feature/blueprint-workflow-nodes` appear without further UI work.
- Keep every existing graph loading, displaying and running unchanged.

## Non-goals

- No change to the graph JSON, saving, validation rules, the runner or the
  backend. `DATA_SCHEMA` stays as it is.
- No change to which nodes exist. Adding and removing nodes (Adapter card,
  Format as text, For each and so on) belongs to the nodes branch.
- No redesign of the Blueprints library page, the Create page, or the
  contents of Test cases and Run history beyond reaching them from the new
  top bar.
- No real undo/redo history if none exists today (see "Left to the builder").

## Anti-goals (a failure even if it technically works)

- Hard-coded hex colours or a second styling system. Every colour is a theme
  token, checked by `--theme-check`.
- A canvas that is prettier but slower or jumpier than today's on graphs of
  30+ nodes.
- Losing keyboard access: every action must still work without a mouse.
- The canvas changing a saved graph without the user doing anything (for
  example, re-saving node positions on open).
- Adding QtQuick, QtWebEngine or any other new Qt module.

## How it works

Approaches considered:

1. **Evolve today's custom-painted `BlueprintCanvas`** and float the new panels
   over it. Lowest risk, but hover, pin hit-testing and per-node interaction
   stay hand-written.
2. **Rebuild the canvas on `QGraphicsView` / `QGraphicsScene` (chosen by the
   owner).** Each node, pin and wire is an item that owns its own painting,
   hover and drag, and Qt provides hit-testing, z-order and transforms. The
   canvas and its tests are rewritten.
3. **Embed the HTML mockup through QtWebEngine.** Rejected: it adds a web
   engine of about 150 MB to every package, bypasses the theme system and its
   checks, and needs a JavaScript bridge.

Structure (names are suggestions):

- `BlueprintScene` (`QGraphicsScene`) builds items from the graph JSON and the
  catalog. `BlueprintView` (`QGraphicsView`) handles pan, zoom, fit, the dotted
  background, rubber-band-free selection and the drop target. It replaces
  `BlueprintCanvas` and keeps that class's outward signals (`nodeSelected`,
  `nodeMoved`, `deleteRequested`, `zoomChanged`, a connect request carrying the
  edge), so `BlueprintPanel` keeps owning the graph and saving.
- `NodeItem`, `PinItem` and `WireItem` are graphics items. Painting uses
  `QPainter` with theme tokens; shadows come from a cached, blurred pixmap per
  node size rather than live effects, to keep scrolling smooth.
- A new connection goes through the existing `BlueprintPreview::connectionError`
  and `BlueprintPanel::addConnection`. Replacing a wire is a remove followed by
  an add. Removing one is a small `removeConnection` on the panel, extracted from
  the Connect dialog's "Remove selected connection". The scene never edits the
  graph itself.
- The floating panels (library dock, inspector, bottom dock, minimap, quick-add,
  details window) are ordinary widgets parented to the view's viewport, placed
  by the view on resize.
- The in-place inspector is a new `BlueprintInspector` widget. It takes the
  editing logic of `configureBlueprintNode` (`blueprintnodedialog.cpp`),
  `editBinding`, `editLiveBinding` and `editDataset`, moved into reusable
  functions rather than copied, and emits node changes that `BlueprintPanel`
  applies through `changeNode`. The read-only parts of today's
  `refreshInspector` (validation messages, port list) move into it too.
- Run on the canvas reuses the run start and polling that Test cases uses
  today (`BlueprintPreview` runs and their step states). The view only maps
  step states onto node items.
- The catalog target is whatever `BlueprintPreview` provides at runtime, on
  `dev` today and after the nodes branch merges. Nothing in the redesign lists
  operations by hand. Families map from the operation's `kind`: input and
  dataset → Sources; transform → Transform; knowledge → Knowledge; model,
  reasoning, adapter and agent → Models; tool → Tools; output → Output; unknown
  kinds → Transform. Legacy-only operations never appear in the library or
  quick-add, but they still render on the canvas. *(Builder's call.)*
- Node explanations for the details window come from a new resource,
  `:/blueprints/node-docs.json`, keyed by operation id: `about`, `example`
  (input label/value, output label/value), `tips`, and per-port notes. It covers
  every non-legacy operation on `dev`. Entries for operations the nodes branch
  removes are harmless, and operations it adds fall back to their catalog
  `detail` and port names until they get an entry, so they still show
  something useful.
- New theme tokens: one colour per data type and one per node family, with dark
  and light values, added to `theme.h` and checked in `Theme::selfCheck()`
  (validity, and at least 3:1 contrast for pins and wires against the canvas).
  Floating-panel fill, panel shadow and the canvas dot colour also become tokens.
- Fullscreen keeps working: the view and its floating panels move into the
  fullscreen window together.

Delivery happens in phases, each one building, passing the checks and leaving
the editor usable:

1. Tokens, then `BlueprintView` and `BlueprintScene` with node, pin and wire
   items, drag-to-connect (including replace and wire selection/delete),
   pan/zoom/fit, selection, deletion and the run states. Swap it in for
   `BlueprintCanvas` with today's toolbar and inspector.
2. The floating shell: top bar with the segmented control and ⋯ menu, bottom
   dock and minimap, with today's inspector content moved into the floating
   panel. Remove the tabs and the fixed right column.
3. The in-place inspector (`BlueprintInspector`), with the configure, binding
   and dataset logic moved into it; the modal dialogs leave the main flow.
4. The library dock (operations, Saved group, **In this workflow**) and the
   quick-add menu with instant adding and context-aware adding from a dropped
   wire. Retire the modal picker.
5. Run on the canvas, and the details window with `node-docs.json`.
6. Light-theme pass, keyboard pass, updated tests and reference screenshots.

## Constraints

- Qt 6 Widgets only; C++20; new `.cpp` files added to `MAGNITE_SOURCES`.
- Never hardcode colours: tokens in `theme.h`, `@token` placeholders in QSS, and
  `Theme::selfCheck()` extended. Type sizes stay on the 11-step scale (11px
  minimum), and DM Sans weights stay at 500 or below.
- Build only with the capped commands
  (`cmake --build build --target magnite-app_autogen -j 4`, then
  `cmake --build build --target magnite-app -j 4`).
- `--theme-check`, `--ui-check` (including the Blueprint checks) and
  `--selftest` must pass at the end of every phase.
- Works in fixture (offline) and live modes, and at the compact 1024×680 window
  size. With `BLUEPRINTS_AND_TOOLS = False` (release builds) nothing changes,
  because the whole workspace is removed.
- Old graphs open as they are, including legacy operations (order example,
  retired Pi node) and nodes the nodes branch later removes from **Add node**.

## Left to the builder

- Exact node metrics, the shadow recipe, wire curvature, and the light-theme
  values of the new tokens (within the contrast rule above).
- Whether a graph saved with today's node sizes needs any spacing adjustment
  on screen. It must never be re-saved automatically.
- Undo/redo: if the editor has no history today, show the buttons disabled or
  leave them out. Don't build history in this project.
- Minimap interaction details beyond click and drag to move the view.
- Where a node lands when added by click (centre of the free canvas area,
  nudged so it doesn't cover another node).

## Merging with the nodes branch

`feature/blueprint-workflow-nodes` will also edit `blueprintpanel.cpp` and the
operation catalog. The redesign stays data-driven so its changes flow in. Expect
one merge between the two branches, best done after phase 4 here, when the panel
code has settled. The redesign does not implement the nodes branch's decisions
(no Adapter card, adapter name on the Model node, removed operations); it only
renders whatever the catalog says.

## Verification

- Blueprint checks rewritten for the new canvas: wires between aligned pins stay
  straight; the dots stay visible; wheel zoom and background pan don't edit node
  positions; fullscreen round-trips; drag-to-connect accepts compatible pins and
  rejects incompatible ones with the existing error text; dropping a wire on empty
  canvas opens a filtered quick-add that connects the new node; deleting a node
  removes its wires.
- Dropping on an occupied input replaces the wire; a `chooseType` port takes the
  wire's type; a selected wire deletes with Delete; Disconnect in the inspector
  removes a wire.
- Inspector edits change the node through `changeNode`, and a port-changing edit
  asks before dropping wires.
- Library, quick-add and details window: search filters, keyboard navigation,
  instant add (connected when from a dropped wire), the Saved group, and a
  details entry for every non-legacy operation.
- Run from the Build view: nodes step through running and done, a failed step
  is marked, the toast links to Run history, and a live draft offers to save a
  revision first.
- Text-fit checks at Default and Large interface sizes, in dark and light, at
  1104px and 800px widths.
- Reference captures of the editor in both themes, compared with the mockup.
