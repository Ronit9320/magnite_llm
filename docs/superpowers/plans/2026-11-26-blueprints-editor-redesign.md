# Blueprints Editor Redesign Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the Blueprints editor's custom-painted canvas, tabs, modal node picker and modal node dialogs with a `QGraphicsView` canvas and borderless floating panels that match `docs/development/blueprints/mockups/editor-v2.html`.

**Architecture:** `BlueprintView` / `BlueprintScene` render the graph JSON as `NodeItem` / `PinItem` / `WireItem` graphics items and emit requests. `BlueprintPanel` still owns the graph and every save (`saveGraph`, `changeNode`, `addConnection`). Floating widgets (top bar, library dock, inspector, bottom dock, minimap, quick-add, details window) are parented to the view's viewport and placed by the view. Everything is drawn from the `BlueprintPreview` catalog at runtime; nothing lists operations by hand.

**Tech Stack:** C++20, Qt 6 Widgets (`QGraphicsView`), the app's `Theme` tokens and `console.qss`, the built-in `--theme-check` / `--ui-check` / `--selftest` checks.

**Spec:** `docs/superpowers/specs/2026-11-25-blueprints-editor-redesign-design.md`

## Global Constraints

- Work on `feature/blueprints-editor-redesign` in `.claude/worktrees/blueprints-editor-redesign`. Never on `dev` or `master`.
- Qt 6 Widgets only. No QtQuick, QtWebEngine or any other new Qt module. C++20.
- Every new `.cpp`/`.h` goes into `MAGNITE_SOURCES` in `app/CMakeLists.txt`, next to the existing `blueprint*` entries.
- Never hardcode hex in widget code or QSS: add a token to `app/src/theme.h`, a `@token` mapping in `Theme::qss()` when QSS uses it, and a check in `Theme::selfCheck()`.
- Type sizes stay on the scale 11/12/13/14/16/18/20/24/28/32/36 (11px minimum); DM Sans weights ≤ 500.
- No change to the graph JSON, saving, validation rules, the runner, the backend or `DATA_SCHEMA`. No change to which operations exist.
- Opening, fitting, zooming or panning a graph never saves it. Only a user action saves.
- Every action works from the keyboard.
- Commit messages are plain sentences in the repo's style (e.g. "Add the Blueprints graphics canvas"). No Co-Authored-By or tool footers.
- Build only with the capped commands, from the worktree root (configure once with `cmake -S app -B build` if `build/` is missing):

  ```bash
  cmake --build build --target magnite-app_autogen -j 4
  cmake --build build --target magnite-app -j 4
  ```

- Fast loop while working on a task (Blueprint checks only):

  ```bash
  QT_QPA_PLATFORM=offscreen MAGNITE_BLUEPRINTS_DESIGN_CHECK=1 ./build/magnite-app --ui-check
  ```

  Expected: every line `[blueprints] ok: …` / `[blueprint-editor] ok: …`, ending `[blueprints] PASS (0 failures)` and `[blueprint-editor] PASS (0 failures)`.
- **Phase gate** (last step of the last task in each phase): all three must pass.

  ```bash
  QT_QPA_PLATFORM=offscreen ./build/magnite-app --theme-check     # "0 failure(s)"
  QT_QPA_PLATFORM=offscreen ./build/magnite-app --ui-check        # 0 failures
  ln -s ../../../.venv .venv && QT_QPA_PLATFORM=offscreen ./build/magnite-app --selftest; rm .venv   # "[selftest] PASS (0 failures)"
  ```

  The `.venv` symlink is temporary; never commit it.

## Review Focus

1. **Opening an old graph must not re-save it.** Opening, fitting, zooming, panning and switching Build/Test cases/Run history leave `preview.saved()` byte-identical (spec anti-goal). Test in Task 5.
2. **An inspector edit committed after the selection changed.** Typing in a field, then clicking another node or pressing Delete, must apply the edit to the node it was typed for (or drop it if that node is gone), never to the newly selected node. Test in Task 9.
3. **Graphs with legacy operations and unknown port types** (order example `selector`/`extractor`/`lookup`/`pricing`, retired `tool`, types `fields`/`order`/`index`, a kind the catalog doesn't know) render, connect and show details without crashing; unknown types colour as text, unknown kinds sit in Transform. Test in Task 2 and Task 13.
4. **40-node graphs stay smooth.** Fit plus 60 full repaints of a 40-node, 60-wire graph stay under 1500 ms offscreen. Test in Task 3.
5. **Compact window (782×628 panel, from the 1024×680 window).** With the library dock open and a node selected, the visible canvas between the dock and the inspector stays at least 240px wide; below that width the library collapses itself. Test in Task 7.

---

## File Structure

New files (all in `app/src/`):

| File | Responsibility |
|---|---|
| `widgets/blueprintstyle.h/.cpp` | Type→colour, kind→family, family→colour/label, node icons, node metrics. Pure functions. |
| `widgets/blueprintitems.h/.cpp` | `NodeItem`, `PinItem`, `WireItem` painting, hover and hit areas. Cached shadow pixmaps. |
| `widgets/blueprintscene.h/.cpp` | `BlueprintScene`: builds items from graph JSON + catalog, node warnings, run states. |
| `widgets/blueprintview.h/.cpp` | `BlueprintView`: pan/zoom/fit, dotted background, selection, drag-to-connect, overlays placement, outward signals. Replaces `BlueprintCanvas`. |
| `widgets/blueprintminimap.h/.cpp` | Minimap overlay. |
| `widgets/floatingpanel.h/.cpp` | Borderless floating panel (fill + cached soft shadow). |
| `panels/blueprinttopbar.h/.cpp` | Top bar: back, name/revision/save state, Draft pill, fixture pill, segmented control, validation, Publish/Reload/Open in Conversation, ⋯ menu, Run ▾. |
| `panels/blueprintnodefields.h/.cpp` | Node editing fields extracted from `NodeDialog` (name, operation setting, response format, instructions, template, ports). |
| `panels/blueprintbindingfields.h/.cpp` | Checkpoint/adapter binding and dataset fields extracted from `editBinding`, `editLiveBinding`, `editDataset`. |
| `panels/blueprintinspector.h/.cpp` | Floating in-place inspector composed of the two field widgets plus wiring list, validation, Test this node, Delete. |
| `panels/blueprintlibrarydock.h/.cpp` | Left dock: Library tab (search, chips, family groups, Saved) and In this workflow tab. |
| `panels/blueprintquickadd.h/.cpp` | Quick-add popup at the cursor. |
| `panels/blueprintdetails.h/.cpp` | Node details window (Overview / Inputs & outputs / Last run). |
| `resources/blueprints/node-docs.json` (in `app/`) | Per-operation explanations. |
| `testing/blueprint_test_support.h` | Shared test helpers moved out of `blueprint_tests.cpp`. |
| `testing/blueprint_editor_tests.h/.cpp` | `runBlueprintEditorChecks()` — all new editor checks. |

Removed at the end of Task 5: `widgets/blueprintcanvas.h/.cpp`. `BlueprintPanel::openNodePicker` and the node tree go in Task 11.

Modified: `theme.h`, `theme.cpp`, `resources/theme/console.qss`, `app/resources.qrc`, `app/CMakeLists.txt`, `panels/blueprintpanel.h/.cpp`, `panels/blueprintpreview.h/.cpp`, `panels/blueprintnodedialog.cpp`, `testing/blueprint_tests.cpp`, `testing/uicheck.cpp`.

## Test harness conventions (used by every task)

- `testing/blueprint_test_support.h` holds `pump`, `visibleNamed<T>`, `visibleButton`, `visibleText`, `drag`, `confirmDelete`, `waitFor`, `waitForState`, `edge` (moved verbatim from the anonymous namespace in `blueprint_tests.cpp`, made `inline`), plus:
  - `void click(QWidget *w, QPointF at, Qt::KeyboardModifiers = {})`
  - `void key(QWidget *w, int key, Qt::KeyboardModifiers = {}, const QString &text = {})`
  - `QPointF viewPoint(BlueprintView &view, QPointF scenePoint)` — `view.mapFromScene(...)` as `QPointF`.
- `blueprint_editor_tests.cpp` has one `check(bool, const char*)` printing `[blueprint-editor] ok:|FAIL: <message>`, and one `static void checkXxx(...)` per task area. `runBlueprintEditorChecks(captures)` sets `Theme::setDark(true)`, builds `BlueprintPreview preview(nullptr, false)`, calls each area, restores the theme and prints `[blueprint-editor] PASS|FAIL (<n> failures)`.
- `uicheck.cpp` calls `runBlueprintEditorChecks(captures)` next to `runBlueprintChecks(captures)` at both call sites: the `MAGNITE_BLUEPRINTS_DESIGN_CHECK` early return becomes `return (runBlueprintChecks(captures) + runBlueprintEditorChecks(captures)) ? 1 : 0;`, and the `Features::blueprintsAndTools` line adds both.
- Fixture graphs in tests are built with `preview.makeNode(op, id, x, y)`. Ports in the fixture catalog: `input` out `text` (text, chooseType); `count` in `value` (array) out `value` (integer); `output` in `value` (text, chooseType); `parse-json` in `value` (text) out `value` (object). `chooseType` lives on the operation spec (`preview.operation(op)`), not on the node — `makeNode` strips it.
- Interaction tests send real `QMouseEvent`/`QKeyEvent`s to `view->viewport()` (offscreen windows don't activate; send focus events directly as `uicheck.cpp` already does for the focus ring).

---

## Phase 1 — The canvas

### Task 1: Editor tokens and `BlueprintStyle`

**Files:**
- Modify: `app/src/theme.h`, `app/src/theme.cpp` (`qss()` map, `selfCheck()`)
- Create: `app/src/widgets/blueprintstyle.h`, `app/src/widgets/blueprintstyle.cpp`
- Create: `app/src/testing/blueprint_test_support.h`, `app/src/testing/blueprint_editor_tests.h`, `app/src/testing/blueprint_editor_tests.cpp`
- Modify: `app/src/testing/blueprint_tests.cpp` (use the support header), `app/src/testing/uicheck.cpp`, `app/CMakeLists.txt`

**Interfaces:**
- Produces (theme.h, each `inline QString`, dark/light):
  `blueprintTypeText, blueprintTypeNumber, blueprintTypeBoolean, blueprintTypeObject, blueprintTypeArray, blueprintTypeReceipt, blueprintTypeAdapter,`
  `blueprintFamilySources, blueprintFamilyTransform, blueprintFamilyKnowledge, blueprintFamilyModels, blueprintFamilyTools, blueprintFamilyOutput,`
  `blueprintPanelFill, blueprintPanelShadow (#AARRGGBB), blueprintCanvasDot`. QSS names: `@blueprintPanelFill`, `@blueprintCanvasDot`.
- Produces (`namespace BlueprintStyle`):
  - `QString typeFamily(const QString &portType)` → one of `text number boolean object array receipt adapter`. Map: `integer|number|index`→number; `object|fields|order`→object; `array`→array; `receipt`→receipt; `adapter-ref`→adapter; `boolean`→boolean; anything else→text.
  - `QColor typeColor(const QString &portType)`
  - `QString nodeFamily(const QString &kind)` → `input|dataset`→sources; `transform`→transform; `knowledge`→knowledge; `model|reasoning|adapter|agent|selector|extractor`→models; `tool`→tools; `output`→output; else transform.
  - `QString familyLabel(const QString &family)` → "Sources", "Transform", "Knowledge", "Models", "Tools", "Output".
  - `QStringList families()` in that order.
  - `QColor familyColor(const QString &family)`
  - `QString nodeIcon(const QString &kind)` — moved from `BlueprintCanvas::nodeIcon` (keep its mapping).
  - `constexpr qreal headerTintAlpha = 0.22;` and node metrics `nodeWidth = 220, headerHeight = 36, pinRowHeight = 24, previewHeight = 22, cornerRadius = 10, pinRadius = 5` (builder may tune; tests read these constants, not literals).

- [ ] **Step 1: Move the helpers into `blueprint_test_support.h`, create the empty editor-check entry point, call it from `uicheck.cpp`, add files to CMake. Build; fast loop passes with `[blueprint-editor] PASS (0 failures)`.**
- [ ] **Step 2: Write failing checks `checkStyle()`**

```cpp
check(BlueprintStyle::typeFamily("integer") == "number" && BlueprintStyle::typeFamily("fields") == "object"
      && BlueprintStyle::typeFamily("adapter-ref") == "adapter" && BlueprintStyle::typeFamily("mystery") == "text",
      "port types map to the seven wire colours, unknown types as text");
check(BlueprintStyle::nodeFamily("dataset") == "sources" && BlueprintStyle::nodeFamily("reasoning") == "models"
      && BlueprintStyle::nodeFamily("tool") == "tools" && BlueprintStyle::nodeFamily("unheard-of") == "transform",
      "node kinds map to families, unknown kinds as Transform");
for (bool dark : {true, false}) { Theme::setDark(dark);
  check(each typeColor(t) for the 7 families has contrast >= 3.0 against Theme::canvasBackground(),
        "wire colours reach 3:1 on the canvas"); }   // reuse a local contrast helper
```

- [ ] **Step 3: Run the fast loop; expect FAIL (does not compile / missing symbols).**
- [ ] **Step 4: Add the tokens and `BlueprintStyle`.** Dark values from the mockup's CSS variables in `editor-v2.html`; light values are the builder's, within the contrast rules. Extend `selfCheck()` in both its dark and light sections: every new token parses as a valid `QColor`; each type token ≥ 3:1 against `canvasBackground()`; `textPrimary()` ≥ 4.5:1 on each family colour blended at `headerTintAlpha` over `blueprintPanelFill()`; `blueprintCanvasDot()` differs from `canvasBackground()`.
- [ ] **Step 5: Build; fast loop and `--theme-check` pass.**
- [ ] **Step 6: Commit** — `git commit -m "Add Blueprints editor tokens and style mapping"`

### Task 2: Graph items and `BlueprintScene`

**Files:**
- Create: `app/src/widgets/blueprintitems.h/.cpp`, `app/src/widgets/blueprintscene.h/.cpp`
- Modify: `app/CMakeLists.txt`, `app/src/testing/blueprint_editor_tests.cpp`

**Interfaces:**
- Consumes: `BlueprintStyle::*` (Task 1); `BlueprintPreview::operation`, `operationId`, `portLabel`, `nodeKindLabel`, `issues`, `fixtures`.
- Produces:
  - `class PinItem : public QGraphicsItem` — `QString nodeId() const; QString portId() const; bool isOutput() const; QString portType() const; QPointF anchor() const` (scene coords of the pin centre); `void setHighlight(Highlight)` with `enum class Highlight { None, Compatible, Dimmed }`.
  - `class WireItem : public QGraphicsPathItem` — `QJsonObject edge() const; void updatePath()` (cubic from output anchor to input anchor, horizontal tangents; a straight line when anchors share y; a faint glow drawn as a wider, low-alpha stroke of the same type colour under the wire); `void setActive(bool)` (animated dash for run); selectable (`ItemIsSelectable`), brightens when selected; `shape()` is a stroker ≥ 10px wide for clicking.
  - `class NodeItem : public QGraphicsObject` — `NodeItem(const QJsonObject &node, const QJsonObject &operation, const BlueprintPreview &preview)`; `QString id() const; QJsonObject node() const; PinItem *pin(bool output, const QString &portId) const; QList<PinItem*> pins() const;` `void setWarning(const QString &text); QString warning() const` (empty clears; shows a warning mark with tooltip); `void setRunStatus(const QString &status); QString runStatus() const` (`""|running|passed|failed|waiting`); `static QSizeF sizeFor(const QJsonObject &node)` (header + max(in,out)·pinRowHeight + previewHeight when the node has a preview + padding). Movable and selectable; position = node `x`,`y`. Header tinted by `familyColor(nodeFamily(kind))` at `headerTintAlpha`, icon, name, operation label (`preview.operation(op)["label"]`, falling back to `nodeKindLabel(kind)`). Preview line: `template` text, else the model label via `BlueprintPreview::modelLabel(node["model"])`, else an input `sample`; elided. Shadow from a pixmap blurred once per `sizeFor` and cached in a static `QHash<QSize, QPixmap>`.
  - `class BlueprintScene : public QGraphicsScene` — `BlueprintScene(const BlueprintPreview &preview, QObject *parent = nullptr); void setGraph(const QJsonObject &graph); QJsonObject graph() const; NodeItem *node(const QString &id) const; QList<NodeItem*> nodes() const; QList<WireItem*> wires() const; static QRectF nodeRect(const QJsonObject &node)` (pos + `NodeItem::sizeFor`). `setGraph` rebuilds all items and computes warnings: `"<Port> is not connected"` for every input port with no edge, plus each `preview.issues(graph)` line that starts with the node's name.

- [ ] **Step 1: Write failing checks `checkScene(preview)`**

```cpp
BlueprintScene scene(preview);
auto a = preview.makeNode("input", "a", 0, 0), b = preview.makeNode("output", "b", 300, 0);
scene.setGraph({{"nodes", QJsonArray{a, b}}, {"edges", QJsonArray{edge("a","text","b","value")}}});
check(scene.nodes().size() == 2 && scene.wires().size() == 1, "the scene builds one item per node and edge");
auto *w = scene.wires().first();
check(qAbs(w->path().boundingRect().height()) <= 1.0, "a wire between aligned pins stays straight");
check(w->pen().color() == BlueprintStyle::typeColor("text"), "wires take the colour of their data type");
check(scene.node("b")->pin(false, "value")->anchor().x() < scene.node("b")->sceneBoundingRect().center().x()
      && scene.node("a")->pin(true, "text")->anchor().x() > scene.node("a")->sceneBoundingRect().center().x(),
      "inputs sit on the left edge and outputs on the right");
scene.setGraph({{"nodes", QJsonArray{b}}});
check(!scene.node("b")->warning().isEmpty(), "an unconnected input shows a warning mark");
scene.setGraph(preview.example());
check(scene.nodes().size() == preview.example()["nodes"].toArray().size(),
      "the legacy order example renders every node");
auto odd = preview.makeNode("output", "odd", 0, 0); odd["kind"] = "unheard-of";
auto in = odd["in"].toArray(); auto p = in[0].toObject(); p["type"] = "mystery"; in[0] = p; odd["in"] = in;
scene.setGraph({{"nodes", QJsonArray{odd}}});
check(scene.node("odd") && scene.node("odd")->pin(false, "value"), "unknown kinds and port types still render");
```

- [ ] **Step 2: Run the fast loop; expect FAIL (missing classes).**
- [ ] **Step 3: Implement the items and scene per the Interfaces block.** Item z-order: wires 0, nodes 1, the dragged wire 2.
- [ ] **Step 4: Build; fast loop passes.**
- [ ] **Step 5: Commit** — `git commit -m "Add Blueprints graph items and scene"`

### Task 3: `BlueprintView` — camera, selection, moving

**Files:**
- Create: `app/src/widgets/blueprintview.h/.cpp`
- Modify: `app/CMakeLists.txt`, `app/resources/theme/console.qss` (`#blueprintView` background), `app/src/testing/blueprint_editor_tests.cpp`

**Interfaces:**
- Consumes: `BlueprintScene` (Task 2).
- Produces `class BlueprintView : public QGraphicsView`:
  - `BlueprintView(const BlueprintPreview &preview, QWidget *parent = nullptr); BlueprintScene *graphScene() const;`
  - `void setGraph(const QJsonObject &graph)` — keeps camera and selection across rebuilds; auto-fits only until the user first pans or zooms (same rule as today's `m_autoFit`).
  - `void setSelection(const QString &nodeId); QString selection() const; void fit(); void zoomBy(qreal factor); int zoomPercent() const; void revealNode(const QString &id)` (centre it); `QPointF centerInGraph() const;`
  - `void setRunStates(const QHash<QString, QString> &statusByNode, const QString &activeNode); void clearRunStates();`
  - Signals (same names and arguments as `BlueprintCanvas` where they exist): `zoomChanged(int percent)`, `nodeSelected(const QString &id)` (empty on clear), `nodeMenuRequested(const QString &id, const QPoint &screen, const QPointF &graph)`, `deleteRequested(const QString &id)`, `nodeMoved(const QString &id, const QPoint &position)` (once per drag, on release, only when the position changed), and new: `canvasMenuRequested(const QPoint &screen, const QPointF &graph)`, `quickAddRequested(const QPoint &screen, const QPointF &graph)`, `detailsRequested(const QString &id)`.
  - Behaviour: dotted background drawn in `drawBackground` with `blueprintCanvasDot` on a 24px grid (dots skipped below 35% zoom, so the grid never becomes noise); wheel zooms at the cursor (0.25×–2.5×); left-drag on background and middle-drag pan; no rubber band; `ViewportUpdateMode::SmartViewportUpdate`, `CacheBackground`. Keys: arrows pan, `+`/`-` zoom, `F` and `Home` fit, `Delete`/`Backspace` → `deleteRequested(selection)` (or the wire disconnect from Task 4), `Space`/`Tab` → `quickAddRequested` at the cursor (centre when the cursor is outside), `i` → `detailsRequested(selection)`. Double-click on background → `quickAddRequested`; right-click on background → `canvasMenuRequested`; right-click on a node → `nodeMenuRequested`. Accessible name "Workflow graph" with today's accessible description, plus the new keys.

- [ ] **Step 1: Write failing checks `checkView(preview)`**

```cpp
BlueprintView view(preview); view.resize(900, 600); view.show(); pump();
const auto graph = preview.example(); view.setGraph(graph); view.fit(); pump();
const int fitted = view.zoomPercent();
// wheel at centre
check(after wheel(+120) at viewport centre: view.zoomPercent() > fitted, "wheel zooms the canvas");
// background pan leaves the graph alone
QJsonObject seen; QObject::connect(&view, &BlueprintView::nodeMoved, [&](auto id, auto){ seen[id] = true; });
drag(view.viewport(), emptyPoint, emptyPoint + QPointF(63, 29));
check(view.centerInGraph() != before && seen.isEmpty(), "dragging the background pans without moving nodes");
// node drag
drag(view.viewport(), centreOf("input"), centreOf("input") + QPointF(40, 0), 30);
check(seen.size() == 1 && seen.contains("input"), "dragging a node reports one move on release");
check(grab has a pixel != canvasBackground in the top-left 30x30, "canvas dots stay visible");
key(view.viewport(), Qt::Key_Delete) after setSelection("input") → deleteRequested("input") emitted,
      "Delete asks to remove the selected node";
// performance (Review Focus 4)
QJsonArray nodes, edges; for i in 0..39: nodes += makeNode(i%2 ? "count" : "input", "n"+i, (i%8)*260, (i/8)*160);
   edges += 60 valid-or-not edges between neighbours (rendering does not validate);
view.setGraph({{"nodes", nodes}, {"edges", edges}}); QElapsedTimer t; t.start();
view.fit(); for (int k = 0; k < 60; ++k) view.viewport()->repaint();
check(t.elapsed() < 1500, "a 40-node graph repaints 60 times in under 1.5 s");
```
`emptyPoint` is found the way today's fullscreen check does (scan for a point whose scene position is outside every `BlueprintScene::nodeRect` grown by 16px); `centreOf(id)` is `view.mapFromScene(view.graphScene()->node(id)->sceneBoundingRect().center())`.

- [ ] **Step 2: Run the fast loop; expect FAIL.**
- [ ] **Step 3: Implement `BlueprintView` per the Interfaces block.**
- [ ] **Step 4: Build; fast loop passes.**
- [ ] **Step 5: Commit** — `git commit -m "Add the Blueprints graphics view with pan, zoom and selection"`

### Task 4: Drag-to-connect, replace-on-drop, chooseType adoption, wire removal

**Files:**
- Modify: `app/src/panels/blueprintpreview.h/.cpp`, `app/src/widgets/blueprintview.h/.cpp`, `app/src/widgets/blueprintitems.cpp`, `app/src/testing/blueprint_editor_tests.cpp`

**Interfaces:**
- Produces on `BlueprintPreview` (all `const`, pure):
  - `bool choosesType(const QJsonObject &node, const QString &side, const QString &portId) const` — reads `chooseType` from `operation(operationId(node))[side]`.
  - `QJsonObject withPortType(const QJsonObject &node, const QString &side, const QString &portId, const QString &type) const` — sets the port's `type` and `schema` (from `contracts[type].schema`).
  - `QJsonObject connectGraph(const QJsonObject &blueprint, const QJsonObject &edge, int *droppedEdges = nullptr, QString *error = nullptr) const` — returns `blueprint` with: the edge already into `edge.to/edge.in` removed (replace); if the target port chooses its type and differs from the source port's type, the target adopts it, else if the source port chooses its type, the source adopts the target's; then any other edge on the retyped node that now fails `connectionError` removed (counted in `*droppedEdges`, replaced edge not counted); then `edge` appended. Returns `{}` and sets `*error` to `connectionError(...)` text when the edge is still invalid.
  - `QString dropError(const QJsonObject &blueprint, const QJsonObject &edge) const` — the `error` from `connectGraph`.
- Produces on `BlueprintView`:
  - `void setDropValidator(std::function<QString(const QJsonObject &edge)> validator)`
  - Signals: `connectRequested(const QJsonObject &edge)`, `disconnectRequested(const QJsonObject &edge)`, `wireDropped(const QString &fromNode, const QString &outPort, const QPoint &screen, const QPointF &graph)`.
  - Behaviour: press on an output pin starts a temporary wire following the cursor; every input pin gets `Highlight::Compatible` when the validator returns empty for that edge, else `Dimmed`. Release on an input pin: empty validator → `connectRequested(edge)`; else nothing changes and a `QLabel#blueprintDropMessage` shows the error near the pin for 2.5 s. Release on empty canvas → `wireDropped`. Press on a wired input pin picks up that wire (the wire detaches visually); release on another input → `disconnectRequested(old)` then `connectRequested(new)`; release on empty canvas → `disconnectRequested(old)`. Click a wire → selects it; `Delete` with a selected wire → `disconnectRequested(wire->edge())`.

- [ ] **Step 1: Write failing checks `checkConnecting(preview)`**

```cpp
auto in = preview.makeNode("input", "in", 0, 0), cnt = preview.makeNode("count", "cnt", 0, 200),
     out = preview.makeNode("output", "out", 320, 0);
QJsonObject g{{"nodes", QJsonArray{in, cnt, out}}, {"edges", QJsonArray{edge("in","text","out","value")}}};
check(preview.dropError(g, edge("in","text","cnt","value")).contains("Text cannot connect to Array"),
      "dropping on an incompatible pin explains why with the existing text");
int dropped = -1; auto r = preview.connectGraph(g, edge("cnt","value","out","value"), &dropped);
check(r["edges"].toArray().size() == 1 && r["edges"].toArray().first().toObject()["from"] == "cnt",
      "dropping on an occupied input replaces its wire");
check(findPort(r, "out", "in", "value")["type"] == "integer" && dropped == 0,
      "a port that chooses its type adopts the wire's type");
BlueprintView view(preview); view.resize(900, 600); view.show();
view.setDropValidator([&](const QJsonObject &e) { return preview.dropError(g, e); }); view.setGraph(g); pump();
QJsonObject asked; connect connectRequested → asked = edge;
drag(viewport, pinPoint("cnt", true, "value"), pinPoint("out", false, "value"));
check(asked == edge("cnt","value","out","value"), "dragging from an output to an input asks to connect");
asked = {}; drag(viewport, pinPoint("in", true, "text"), pinPoint("cnt", false, "value"));
check(asked.isEmpty() && visible QLabel "blueprintDropMessage" text contains "cannot connect",
      "an incompatible drop connects nothing and shows the reason");
bool canvasDrop = false; connect wireDropped → canvasDrop = (fromNode == "cnt");
drag(viewport, pinPoint("cnt", true, "value"), emptyPoint); check(canvasDrop, "dropping a wire on empty canvas is reported");
QJsonObject removed; connect disconnectRequested → removed = edge;
click(viewport, wireMidpoint(0)); key(viewport, Qt::Key_Delete);
check(removed == edge("in","text","out","value"), "a selected wire is removed with Delete");
```
`findPort(graph, node, side, port)` and `pinPoint(node, output, port)` / `wireMidpoint(i)` are small local helpers in the test file.

- [ ] **Step 2: Run the fast loop; expect FAIL.**
- [ ] **Step 3: Implement the preview functions, then the view interaction.**
- [ ] **Step 4: Build; fast loop passes.**
- [ ] **Step 5: Commit** — `git commit -m "Connect Blueprint nodes by dragging wires"`

### Task 5: Swap the new canvas into `BlueprintPanel`

**Files:**
- Modify: `app/src/panels/blueprintpanel.h/.cpp`, `app/src/testing/blueprint_tests.cpp`, `app/src/testing/blueprint_editor_tests.cpp`, `app/CMakeLists.txt`
- Delete: `app/src/widgets/blueprintcanvas.h/.cpp`

**Interfaces:**
- Consumes: `BlueprintView` signals and methods (Tasks 3–4); `BlueprintPreview::connectGraph`, `dropError`.
- Produces on `BlueprintPanel` (private):
  - `bool dropConnection(const QJsonObject &edge)` — `connectGraph`; if `droppedEdges > 0`, `Ui::confirmAction(..., "Update node", "This change will remove N incompatible connection(s).", "Save changes")` (same copy as `changeNode`, built with `countOf`); then `saveGraph`. Shows `Ui::showNotice(..., "Connect ports", error)` on an error.
  - `bool removeConnection(const QJsonObject &edge)` — removes the edge matching all four fields; `saveGraph`. The Connect dialog's "Remove selected connection" calls it.
  - `BlueprintView *m_canvas` replaces `BlueprintCanvas *m_canvas`. `addNode` uses `BlueprintScene::nodeRect`. The `portSelected` click-to-connect flow is removed; `connectPorts` (the dialog) stays in the node menu.
  - Wiring: `connectRequested → dropConnection`; `disconnectRequested → removeConnection`; `wireDropped`, `quickAddRequested`, `canvasMenuRequested` → `openNodePicker(graphPos)` for now (Task 11 replaces it); `detailsRequested` ignored until Task 13.

- [ ] **Step 1: Port the canvas checks in `blueprint_tests.cpp`.** Replace every `BlueprintCanvas` with `BlueprintView` (`findChild<BlueprintView *>()`), `BlueprintCanvas::nodeRect` with `BlueprintScene::nodeRect`, and sends to `canvas` with sends to `canvas->viewport()`. Delete the stand-alone straight-cable/dots block (now in Tasks 2–3). Keep every other expectation as it is.
- [ ] **Step 2: Add failing panel checks `checkPanelCanvas(preview)` in the editor tests**

```cpp
BlueprintPanel panel(&preview); panel.resize(1120, 720); panel.show();
preview.saveBlueprint(preview.example()); open "Order intake" from the library;
const auto before = preview.saved();
auto *view = panel.findChild<BlueprintView *>(); view->fit(); wheel zoom; background pan; pump(200);
check(preview.saved() == before, "opening, fitting, zooming and panning never re-save the graph");
drag from an output pin onto an occupied, compatible input in the example graph;
check(the stored graph has the new edge and not the old one, edges count unchanged,
      "dropping on an occupied input replaces its wire in the saved graph");
select that wire, press Delete;
check(the stored graph no longer has it, "Delete removes the selected wire from the saved graph");
```
- [ ] **Step 3: Run the fast loop; expect FAIL.**
- [ ] **Step 4: Swap the canvas in, add `dropConnection`/`removeConnection`, delete `blueprintcanvas.*` and its CMake entries.**
- [ ] **Step 5: Build; fast loop passes (old and new checks).**
- [ ] **Step 6: Phase gate** (Global Constraints). Capture `--screenshot` of the editor to the scratchpad and compare against the mockup by eye.
- [ ] **Step 7: Commit** — `git commit -m "Use the graphics canvas in the Blueprints editor"`

---

## Phase 2 — The floating shell

### Task 6: Floating panels, bottom dock and minimap

**Files:**
- Create: `app/src/widgets/floatingpanel.h/.cpp`, `app/src/widgets/blueprintminimap.h/.cpp`
- Modify: `app/src/widgets/blueprintview.h/.cpp`, `app/resources/theme/console.qss`, `app/src/testing/blueprint_editor_tests.cpp`, `app/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `class FloatingPanel : public QFrame` — `objectName "blueprintFloating"`, fill `@blueprintPanelFill`, no border, radius 12, soft shadow painted from a cached blurred pixmap (rebuilt only on resize) using `blueprintPanelShadow`. `contentsMargins` leave room for the shadow.
  - `enum class OverlayEdge { TopLeft, Top, TopRight, Left, Right, BottomLeft, Bottom, BottomRight }`; on `BlueprintView`: `void addOverlay(QWidget *w, OverlayEdge edge, int margin = 12)`; `QRect freeArea() const` (viewport rect minus visible Left/Right overlays — used for "add at centre" and the details window); overlays re-placed on resize and on `QEvent::Show/Hide/LayoutRequest` of an overlay.
  - `class BlueprintMinimap : public QWidget` — `BlueprintMinimap(BlueprintView *view)`; paints node rects (family colour) and the visible area; click/drag centres the view there. Fixed 180×112.
  - Bottom dock (built in `BlueprintView` as a `FloatingPanel` at `Bottom`): buttons with accessible names "Show node library" (checkable), "Add node", "Zoom out", zoom % label, "Zoom in", "Fit graph". Signals on the view: `libraryToggled(bool)`, `addNodeClicked()`. No undo/redo buttons: the editor has no history (spec, "Left to the builder").

- [ ] **Step 1: Write failing checks `checkOverlays(preview)`** — view 900×600 with the example graph:

```cpp
check(view has visible buttons "Add node", "Zoom in", "Zoom out", "Fit graph", "Show node library",
      "the bottom dock carries add, zoom and fit");
auto *mini = view.findChild<BlueprintMinimap *>();
check(mini && mini->isVisible() && mini->geometry().bottomRight() is within 16px of the viewport's,
      "the minimap sits in the bottom-right corner");
const auto c0 = view.centerInGraph(); click(mini, mini->rect().topLeft() + QPointF(10, 10));
check(view.centerInGraph() != c0, "clicking the minimap moves the view");
click "Zoom in"; check(view.zoomPercent() > previous, "Zoom in zooms");
check(QColor of a FloatingPanel's centre pixel != canvasBackground && panel has no 1px border line
      (pixel at the panel's edge+1 equals the panel fill or shadow, not surfaceBorder), "floating panels have no border");
```
- [ ] **Step 2: Run; expect FAIL.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Build; fast loop passes.**
- [ ] **Step 5: Commit** — `git commit -m "Add floating panels, the bottom dock and the minimap to the Blueprints canvas"`

### Task 7: Top bar, segmented views, floating inspector shell, fullscreen

**Files:**
- Create: `app/src/panels/blueprinttopbar.h/.cpp`
- Modify: `app/src/panels/blueprintpanel.h/.cpp`, `app/resources/theme/console.qss`, `app/src/testing/blueprint_tests.cpp`, `app/src/testing/blueprint_editor_tests.cpp`, `app/CMakeLists.txt`

**Interfaces:**
- Produces `class BlueprintTopBar : public FloatingPanel`:
  - `enum class View { Build, TestCases, RunHistory }; void setView(View); View view() const;`
  - `void setBlueprint(const QString &name, const QString &revisionText, bool draft, bool fixture)`; `void setValidation(const QStringList &issues)` → "Ready to run" or `countOf(n, "issue")`, clicking it selects the first offending node (signal `issueClicked(int index)`); `void setActions(bool canPublish, bool canReload, bool canOpenInConversation, bool canGoBackToConversation)`; `void setRunEnabled(bool)`.
  - Signals: `backClicked()`, `viewChanged(View)`, `publishClicked()`, `reloadClicked()`, `openInConversationClicked()`, `runClicked()`, `runMenuRequested(QPoint)` (the ▾), `editDetailsClicked()`, `duplicateClicked()`, `deleteClicked()`, `backToConversationClicked()`.
  - Accessible names: "Build", "Test cases", "Run history" (a checkable button group), "Run", "Run options", "More blueprint actions" (the ⋯), "Back to library". The ⋯ menu items: "Edit details…", "Duplicate", "Delete blueprint…", and "Back to Conversation" only when allowed.
- Panel changes: the `TabWidget m_tabs` is replaced by a `QStackedWidget` driven by `BlueprintTopBar::viewChanged`; Test cases and Run history pages keep their existing contents. The top bar is an overlay at `Top` in Build; in the other two views it sits above their page (same widget, re-parented by the panel). Today's `m_inspector` content moves into a `FloatingPanel` overlay at `Right` (width 300, scrollable), visible only while a node is selected. The node tree moves into a `FloatingPanel` overlay at `Left` (library dock slot) until Task 10. The fixed right column and the old toolbar row are removed; `m_title`, `m_revision`, `m_publish`, `m_reload`, `m_openInConversation`, `m_backToChat` are replaced by the top bar API. Fullscreen re-parents the view (with its overlays and the top bar) into the fullscreen dialog and back.
- Compact rule (Review Focus 5): when a node is selected and `freeArea().width() < 240`, the left dock hides and the "Show node library" toggle unchecks; it does not reopen by itself.

- [ ] **Step 1: Update `blueprint_tests.cpp`** for the new shell: tab clicks become top bar clicks ("Test cases", "Run history", "Build"); "Node inspector" text is now the inspector heading in the floating panel; `Publish`/`Back to Conversation` are found in the top bar or ⋯ menu. Keep every behavioural expectation.
- [ ] **Step 2: Write failing checks `checkShell(preview)`**

```cpp
open the example in BlueprintPanel 1120x720;
check(no visible QTabWidget in panel && visible buttons "Build", "Test cases", "Run history", "Run",
      "a segmented control replaces the editor tabs");
check(top bar text contains "Order intake" && validation label text is "Ready to run" or "<n> issue(s)",
      "the top bar shows the name and live validation");
check(the inspector FloatingPanel is hidden, "the inspector is hidden with nothing selected");
view->setSelection(first node id) via a click; check(inspector visible && its right edge within 16px of the viewport's right,
      "selecting a node floats the inspector in on the right");
open ⋯ menu; check(actions contain "Edit details…", "Duplicate", "Delete blueprint…", "the ⋯ menu holds blueprint actions");
panel.resize(782, 628); select a node; pump();
check(view->freeArea().width() >= 240, "the compact window keeps a usable canvas beside the inspector");
fullscreen round trip: check(view->window()->isFullScreen() && top bar is a descendant of view->window(), then back,
      "the view, its panels and the top bar move into fullscreen and back");
```
- [ ] **Step 3: Run; expect FAIL.**
- [ ] **Step 4: Implement the top bar and the shell changes.**
- [ ] **Step 5: Build; fast loop passes.**
- [ ] **Step 6: Phase gate.**
- [ ] **Step 7: Commit** — `git commit -m "Float the Blueprints editor shell over the canvas"`

---

## Phase 3 — The in-place inspector

### Task 8: Extract node, binding and dataset fields

**Files:**
- Create: `app/src/panels/blueprintnodefields.h/.cpp`, `app/src/panels/blueprintbindingfields.h/.cpp`
- Modify: `app/src/panels/blueprintnodedialog.cpp`, `app/src/panels/blueprintpanel.cpp` (`editBinding`, `editLiveBinding`, `editDataset` become thin dialogs over the new widgets), `app/src/testing/blueprint_editor_tests.cpp`, `app/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `class BlueprintNodeFields : public QWidget` — `BlueprintNodeFields(BlueprintPreview &preview, const QString &kind, const QJsonObject &node, QWidget *parent = nullptr)`. Contains what `NodeDialog` edits today (operation, name, setting line/choice, response format, instructions, template, port editors) with the same accessible names (`"Output port name"`, `"Output port type"`, `"Input port key"`, …). `QJsonObject node() const` (the edited node, built exactly as `NodeDialog::result()` builds it today); `QString error() const` (the dialog's current validation message, empty when valid); `bool portsChanged() const` (ids or types differ from the original). Signal `edited()` fires on each commit: `editingFinished` for line edits, focus-out for plain-text edits, `currentIndexChanged` for combos, add/remove port.
  - `NodeDialog` keeps its title, buttons and `exec()` contract but hosts one `BlueprintNodeFields`; `configureBlueprintNode` is unchanged for callers.
  - `class BlueprintBindingFields : public QWidget` — `BlueprintBindingFields(BlueprintPreview &preview, const QJsonObject &graph, const QJsonObject &node, QWidget *parent = nullptr)`; fixture and live modes, same choices as `editBinding` / `editLiveBinding` (models, compatible adapters via `compatibleAdapters`, live checkpoint check via `preview.checkpoint`); `QJsonObject node() const`; signal `edited()`.
  - `class BlueprintDatasetFields : public QWidget` — same contract for `editDataset`.
  - The three existing dialogs host these widgets and keep their accessible names and results, so current flows and checks keep working.

- [ ] **Step 1: Write failing checks `checkFieldWidgets(preview)`**

```cpp
auto node = preview.makeNode("input", "in", 0, 0);
BlueprintNodeFields fields(preview, "input", node);
auto *name = visibleNamed<QLineEdit>(fields, "Output port name"); // fields.show() first
name->setText("Payload"); emit editingFinished; 
check(fields.node()["out"].toArray().first().toObject()["name"] == "Payload" && fields.error().isEmpty(),
      "node fields edit a port name outside the dialog");
type combo "Output port type" → "object"; check(fields.portsChanged(), "changing a port type is reported as a port change");
auto model = first operation with kind "model" → makeNode; BlueprintBindingFields binding(preview, preview.example(), model);
pick the second model; check(binding.node()["model"] != model["model"], "binding fields change the checkpoint");
```
- [ ] **Step 2: Run; expect FAIL.**
- [ ] **Step 3: Move the logic.** Cut the bodies out of `NodeDialog`, `editBinding`, `editLiveBinding`, `editDataset` into the widgets (move, don't copy); the dialogs become hosts.
- [ ] **Step 4: Build; fast loop passes, including the existing dialog-driven checks in `blueprint_tests.cpp` unchanged.**
- [ ] **Step 5: Commit** — `git commit -m "Extract Blueprint node, binding and dataset fields from their dialogs"`

### Task 9: `BlueprintInspector` edits in place

**Files:**
- Create: `app/src/panels/blueprintinspector.h/.cpp`
- Modify: `app/src/panels/blueprintpanel.h/.cpp` (`refreshInspector` delegates; Configure/binding/dataset buttons removed from the inspector), `app/src/testing/blueprint_tests.cpp`, `app/src/testing/blueprint_editor_tests.cpp`, `app/CMakeLists.txt`

**Interfaces:**
- Consumes: `BlueprintNodeFields`, `BlueprintBindingFields`, `BlueprintDatasetFields` (Task 8).
- Produces `class BlueprintInspector : public QWidget` (inside the right `FloatingPanel`):
  - `BlueprintInspector(BlueprintPreview &preview, QWidget *parent = nullptr); void setNode(const QJsonObject &graph, const QString &nodeId)` (empty id clears); `QString nodeId() const`.
  - Content top to bottom: header (icon, name, operation, ⓘ "Node details", close "Clear node selection"); validation messages for the node; the field widgets that apply to the node's kind; test value; "Inputs & outputs" list — each port row shows what it is wired to and a "Disconnect" button per wired row; "Test this node"; "Save to node library…"; "Delete node".
  - Signals: `nodeChanged(const QJsonObject &node, bool portsChanged)`, `disconnectRequested(const QJsonObject &edge)`, `testRequested(const QString &id)`, `saveToLibraryRequested(const QString &id)`, `deleteRequested(const QString &id)`, `detailsRequested(const QString &id)`, `closeRequested()`.
  - Stale commit rule (Review Focus 2): each emitted node carries the id captured when the fields were built; `setNode` with a different id first flushes a pending edit (by clearing focus inside the inspector) for the old id, then rebuilds. The panel applies `nodeChanged` through `changeNode(node, portsChanged)` only if that id still exists in `m_graph`.
  - Rebuilds keep focus: when `setNode` is called for the same id after a save, the focused field keeps focus and cursor position (rebuild only the parts whose data changed, or restore focus by accessible name).

- [ ] **Step 1: Update `blueprint_tests.cpp`** where it drove "Configure node…" for inline-editable things; the node menu still offers "Configure node…", so menu-based checks stay.
- [ ] **Step 2: Write failing checks `checkInspector(preview)`**

```cpp
open a blank blueprint in BlueprintPanel with nodes in(input) → out(output) connected;
select "in"; set "Output port name" to "Payload", press Enter;
check(stored node "in" out[0].name == "Payload", "editing a field in the inspector saves through changeNode");
add p(parse-json) and f(field) with edge p.value → f.value (object → object; field's input does not choose its type);
select "p"; change its "Output port type" to "text"; answer the confirmation with Cancel;
check(stored graph unchanged, "a port change that breaks wires asks first and Cancel keeps everything");
repeat and accept; check(the p→f edge is gone, "accepting drops the broken wire");
select "f"; click "Disconnect" on its wired input row (re-wire first); check(edge gone, "Disconnect removes a wire from the inspector");
// Review Focus 2
select "in"; type "Draft" into "Output port name" without committing; click node "out" on the canvas;
check(stored "in".out[0].name == "Draft" && stored "out" unchanged, "an edit commits to the node it was typed for when selection moves");
select "in"; type "Gone"; press Delete on the canvas with "in" selected and confirm;
check(no node "in" && "out" unchanged, "an edit on a deleted node is dropped");
```
- [ ] **Step 3: Run; expect FAIL.**
- [ ] **Step 4: Implement the inspector; delete the inspector-building code from `refreshInspector` and the inspector's dialog buttons.** The dialogs remain reachable from the node menu ("Configure node…") only.
- [ ] **Step 5: Build; fast loop passes.**
- [ ] **Step 6: Phase gate.**
- [ ] **Step 7: Commit** — `git commit -m "Edit Blueprint nodes in place in the floating inspector"`

---

## Phase 4 — Library and quick-add

### Task 10: Node library dock

**Files:**
- Create: `app/src/panels/blueprintlibrarydock.h/.cpp`
- Modify: `app/src/panels/blueprintpanel.h/.cpp` (dock replaces the node-tree overlay), `app/src/testing/blueprint_tests.cpp`, `app/src/testing/blueprint_editor_tests.cpp`, `app/CMakeLists.txt`

**Interfaces:**
- Consumes: `BlueprintStyle::nodeFamily/familyLabel/families/familyColor/nodeIcon`; `preview.fixtures()["operations"]`, `preview.nodeLibrary()`, `removeNodeTemplate`, `nodeLibraryChanged`.
- Produces `class BlueprintLibraryDock : public QWidget` (inside the left `FloatingPanel`, width 260):
  - `struct Entry { QString operation; QJsonObject savedNode; QString savedId; }` — exactly one of `operation` / `savedNode` is set.
  - `static QList<QPair<QString, QList<Entry>>> groups(const BlueprintPreview &preview, const QString &query, const QString &family)` — families in `BlueprintStyle::families()` order then "Saved"; excludes `legacyOnly` operations and the order-example ones (`selector`, `extractor`, `lookup`, `pricing`); `query` matches label or detail, case-insensitive; empty groups omitted. Shared with quick-add.
  - Tabs "Library" / "In this workflow" (accessible names). Library: search `QLineEdit` (accessible "Search nodes"), chips "All" + families + "Saved", group headers, rows (icon, label, one-line detail, ⓘ). Row click → `addRequested(Entry)`; row drag → a `QDrag` with MIME `application/x-magnite-blueprint-entry` (JSON of the entry), accepted by `BlueprintView` which emits `entryDropped(QByteArray json, QPointF graph)`. Saved rows have "Remove from node library".
  - In this workflow: one row per graph node (icon, name, operation, "needs input" marker when the scene has a warning for it); click → `nodeActivated(id)`. `void setGraph(const QJsonObject &graph, const QHash<QString, QString> &warnings)`.
  - `/` anywhere in the editor focuses the search (shortcut owned by the panel).
- Panel: `Entry` → node via `makeNode(op, BlueprintPreview::id(), x, y)` or the saved node copy with a fresh id (as `openNodePicker` does today); placed at the centre of `freeArea()` mapped to the scene, nudged by `addNode`'s existing slot search; `addNode` then selects it (inspector opens). `nodeActivated` → `setSelection` + `revealNode`. The `QTreeWidget m_nodeTree` and `refreshNodeTree` are removed.

- [ ] **Step 1: Update `blueprint_tests.cpp`**: workflow-tree checks (`"Workflow nodes and connections"`) become In this workflow checks (row count, current row); the "Add Input"/"Add Dataset"/"Add Transform" picker checks become library-row checks.
- [ ] **Step 2: Write failing checks `checkLibrary(preview)`**

```cpp
const auto all = BlueprintLibraryDock::groups(preview, {}, {});
check(no Entry has operation in {"selector","extractor","lookup","pricing","tool"},
      "legacy operations never appear in the library");
check(the "Sources" group contains "input" and "dataset-rows", "operations are grouped by family");
check(BlueprintLibraryDock::groups(preview, "json", {}) has exactly one row, operation "parse-json",
      "search filters operations by label and detail");
preview.saveNodeTemplate("Object input", makeNode("input",...)); 
check(groups(...).last().first == "Saved" with one entry, "saved nodes form a Saved group");
in the panel: click the "Parse JSON" row; check(the graph gains one parse-json node, selected, inside freeArea,
      "clicking a library row adds the node at once at the centre");
drag the "Count" row onto the canvas at viewport (300, 300);
check(the new count node's scene pos is within 40px of mapToScene(300,300), "dragging a row drops the node there");
In this workflow tab: check(row count == node count && the output node row shows "needs input"
      when unconnected, "In this workflow lists the graph's nodes and what needs input");
click a row; check(view->selection() == that id, "clicking a workflow row selects that node");
```
- [ ] **Step 3: Run; expect FAIL.**
- [ ] **Step 4: Implement.**
- [ ] **Step 5: Build; fast loop passes.**
- [ ] **Step 6: Commit** — `git commit -m "Add the Blueprints node library dock"`

### Task 11: Quick-add menu and context-aware adding; retire the picker

**Files:**
- Create: `app/src/panels/blueprintquickadd.h/.cpp`
- Modify: `app/src/panels/blueprintpanel.h/.cpp` (remove `openNodePicker` and its dialog code), `app/src/testing/blueprint_tests.cpp`, `app/src/testing/blueprint_editor_tests.cpp`, `app/CMakeLists.txt`

**Interfaces:**
- Consumes: `BlueprintLibraryDock::groups` and `Entry` (Task 10); `BlueprintPreview::dropError` (Task 4).
- Produces `class BlueprintQuickAdd : public FloatingPanel` (child of the viewport, not a window):
  - `void open(const QPoint &viewportPos, std::function<bool(const BlueprintLibraryDock::Entry &)> filter = {})` — search field (focused, accessible "Search nodes to add") and grouped list; ↑/↓ move, Enter accepts, Shift+Enter → `detailsRequested(Entry)`, Esc or click outside closes. Width 300, max height 360, flipped to stay inside the viewport.
  - Signals: `accepted(const BlueprintLibraryDock::Entry &entry)`, `detailsRequested(const BlueprintLibraryDock::Entry &entry)`, `closed()`.
- Panel:
  - `quickAddRequested`, `canvasMenuRequested`, "Add node" (bottom dock), and an empty canvas's "Start with a node" button open quick-add at that point with no filter.
  - `wireDropped(from, out, screen, graph)` opens it with a filter: an entry passes when its node (built with `makeNode`) has an input port `p` for which `preview.dropError(graphWithCandidate, edge(from, out, candidateId, p))` is empty. On accept: add the node at the drop point and `dropConnection` to the first such port, in one `saveGraph` (build the graph with the node, then `connectGraph`, then save).
  - The modal picker (`openNodePicker`, `Add node picker`, `blueprintNodeCard` QSS) is deleted.

- [ ] **Step 1: Update `blueprint_tests.cpp`**: the picker checks ("Add node opens a separate themed dialog", "lays out built-in cards across columns", "Page 1 of 2" saved-node paging, "choosing a palette item inserts and selects a node") become quick-add equivalents: "Add node" opens a visible `BlueprintQuickAdd`; typing "input" + Enter inserts and selects an Input node; saved nodes appear in quick-add search. Configure-via-dialog-on-add expectations are replaced by setting the port in the inspector afterwards (adding is instant).
- [ ] **Step 2: Write failing checks `checkQuickAdd(preview)`**

```cpp
panel with nodes in(input) only; press Space with the cursor over empty canvas;
auto *qa = panel.findChild<BlueprintQuickAdd *>(); check(qa && qa->isVisible() && search has focus,
      "Space opens quick-add at the cursor with the search focused");
type "count"; Enter; check(graph has a count node and it is selected && qa hidden, "Enter adds the highlighted node");
Esc test: open, Esc; check(!qa->isVisible() && node count unchanged, "Esc closes quick-add without adding");
drag from in.text to empty canvas; check(qa visible && every listed entry accepts text
      (no "count", which needs an array) && "output" listed, "a dropped wire filters quick-add to compatible nodes");
choose "output"; check(graph has edge in.text → <new output>.value, "the node added from a dropped wire arrives connected");
check(no QDialog named "Add node picker" exists anywhere after clicking "Add node", "the modal node picker is gone");
```
- [ ] **Step 3: Run; expect FAIL.**
- [ ] **Step 4: Implement; delete the picker.**
- [ ] **Step 5: Build; fast loop passes.**
- [ ] **Step 6: Phase gate.** This is the point to merge `feature/blueprint-workflow-nodes` (spec, "Merging with the nodes branch"); **ask the owner before merging** — it is not part of this task.
- [ ] **Step 7: Commit** — `git commit -m "Add nodes from a quick-add menu and retire the node picker"`

---

## Phase 5 — Run on the canvas, details window

### Task 12: Run from the Build view

**Files:**
- Modify: `app/src/panels/blueprintpanel.h/.cpp` (`startRun` split), `app/src/widgets/blueprintview.cpp`, `app/src/widgets/blueprintitems.cpp`, `app/src/testing/blueprint_editor_tests.cpp`

**Interfaces:**
- Consumes: `BlueprintTopBar::runClicked/runMenuRequested` (Task 7); `BlueprintView::setRunStates/clearRunStates` (Task 3); existing `m_target`, `m_sample`, `m_scenario`, `m_input` in Test cases; `BlueprintPreview::start`, `runLive`, `run`, `runChanged`.
- Produces on `BlueprintPanel` (private):
  - `void runFromCanvas()` — uses the Test cases selection (first sample/test case when none chosen) and calls the same start path as `startRun` (extract the shared body into `bool beginRun(QString *error)`; `startRun` keeps its trace behaviour). Live mode with a full run of an unsaved draft: `Ui::confirmAction(..., "Save a revision", "A full run needs a saved revision. Save one now?", "Save and run")` → `publish()` then run on success. Draft test runs need no prompt.
  - `void showCanvasRun(const QJsonObject &run)` — maps `run["steps"]` (each step's `id` is the node id) to statuses for `setRunStates`; the active node is the step at `run["cursor"]` (live) or the first `running` step. Wires into the active node animate (`WireItem::setActive`). Done nodes show a ✓, failed ones the error mark. Called from the existing `runChanged` handling when `run["id"] == m_runId`.
  - Toast: a `FloatingPanel` overlay at `Bottom`, above the dock, accessible name "Run result", text `"<Status> · <duration>"` and an "Open in Run history" link button that switches to Run history with that run selected; auto-hides after 6 s or on Esc.
  - Run ▾ menu: the available test cases/samples (checkable, current one checked) and "Open Test cases…".
  - Opening another blueprint or leaving the editor clears run states (`m_navigation` already advances; check it in `showBlueprint`).

- [ ] **Step 1: Write failing checks `checkCanvasRun(preview)`** (fixture mode, the order example)

```cpp
open the example; click top bar "Run";
check(waitFor([&]{ some NodeItem has runStatus() == "running"; }), "Run starts from the Build view and shows the running node");
check(waitForState(preview, runId, "passed") && every step node's runStatus() == "passed",
      "nodes step through to done");
check(visible FloatingPanel "Run result" with an "Open in Run history" button, "a toast reports the result");
click it; check(top bar view == RunHistory, "the toast opens the run in Run history");
choose the failing scenario via Run ▾ (the one today's checks use for "output.quantity must be an integer"); run;
check(waitForState(..., "failed") && the failing step's node runStatus() == "failed", "a failed step is marked on the canvas");
open another blueprint; check(no NodeItem has a non-empty runStatus(), "run marks clear when another blueprint opens");
```
The live "offer to save a revision" path is covered in the live Blueprint checks (`runLiveBlueprintChecks`): add one step there that clicks Run on a draft and accepts the prompt, expecting a new revision and a run with that revision.
- [ ] **Step 2: Run; expect FAIL.**
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Build; fast loop passes.**
- [ ] **Step 5: Commit** — `git commit -m "Run Blueprints from the canvas"`

### Task 13: Node details window and `node-docs.json`

**Files:**
- Create: `app/resources/blueprints/node-docs.json`, `app/src/panels/blueprintdetails.h/.cpp`
- Modify: `app/resources.qrc`, `app/src/panels/blueprintpanel.cpp`, `app/src/widgets/blueprintitems.cpp` (ⓘ on hover/selection), `app/src/testing/blueprint_editor_tests.cpp`, `app/CMakeLists.txt`

**Interfaces:**
- `node-docs.json`: `{ "<operation id>": { "about": "...", "example": { "inputLabel": "...", "input": "...", "outputLabel": "...", "output": "..." }, "tips": ["..."], "ports": { "in.<id>": "...", "out.<id>": "..." } } }`, one entry for every non-legacy operation in the fixture catalog **and** the live catalog on `dev` (`generate`, `reason`, `lora`, `knowledge` operations and the rest added in the live branch of the `BlueprintPreview` constructor). Plain, short copy in the app's voice.
- Produces `class BlueprintDetails : public FloatingPanel` (child of the viewport, draggable by its header, close button, Esc):
  - `void showFor(const QString &operation, const QJsonObject &node /* may be empty for library entries */, const QJsonObject &graph, const QJsonObject &lastStep, const QRect &anchor)`.
  - Tabs "Overview", "Inputs & outputs", "Last run". Overview: docs `about`, the example as two labelled blocks, tips; falls back to the catalog `detail` when the operation has no entry. Inputs & outputs: each port name + type chip (`typeColor`) + docs note, and for a canvas node what it is wired to. Last run: status, duration, output (`BlueprintPreview::jsonText`) of the latest step for that node in the latest run of this blueprint (`preview.runs(id)`), or "Not run yet".
  - Placement: inside `BlueprintView::freeArea()`, beside `anchor`, never over the inspector or library dock; width 340.
- Entry points: ⓘ on a node (hover or selected) → `detailsRequested(id)`; `i` key; ⓘ in the inspector header; ⓘ on library rows; Shift+Enter in quick-add.

- [ ] **Step 1: Write failing checks `checkDetails(preview)`**

```cpp
const auto docs = parse ":/blueprints/node-docs.json";
for (each operation in preview.fixtures()["operations"] that is not legacyOnly and not an order-example op)
  check(docs.contains(id) && !docs[id]["about"].toString().isEmpty(), "every library operation has an explanation"); // one aggregated check
select a node; key 'i'; auto *d = panel.findChild<BlueprintDetails *>();
check(d && d->isVisible() && tab "Overview" shows docs about text, "i opens the details window for the selected node");
check(d->geometry() does not intersect the inspector's or library dock's geometry && is inside view->freeArea(),
      "the details window opens in the free canvas area");
switch to "Inputs & outputs"; check(shows the wired source node's name for a connected input, "details show what each pin is wired to");
switch to "Last run" before any run: text "Not run yet"; run the example, reopen: shows "Passed", "Last run shows the latest step");
showFor("unheard-op", {}, {}, {}, {}) → check(visible and shows fallback text without crashing, "an operation without docs still shows details");
```
- [ ] **Step 2: Run; expect FAIL.**
- [ ] **Step 3: Write `node-docs.json`, register it in `resources.qrc`, implement the window and its entry points.**
- [ ] **Step 4: Build; fast loop passes.**
- [ ] **Step 5: Phase gate.**
- [ ] **Step 6: Commit** — `git commit -m "Add the Blueprint node details window"`

---

## Phase 6 — Polish and proof

### Task 14: Light theme, keyboard, text fit and reference captures

**Files:**
- Modify: whichever editor files need fixes; `app/src/testing/blueprint_editor_tests.cpp`; `app/src/testing/blueprint_tests.cpp` (capture names); `docs/development/blueprints/mockups/README.md` (point at the shipped editor)

**Interfaces:** none new.

- [ ] **Step 1: Write failing checks `checkPolish(preview, captures)`**

```cpp
for (bool dark : {true, false}) for (auto size : {Default, Large interface size}) for (int width : {1104, 800}) {
  set theme and the interface-size preference the way uicheck.cpp's text-fit checks do; panel.resize(width, 720);
  open the example, select a node, open quick-add and details in turn;
  check(no QLabel/QAbstractButton inside the editor overlays has text wider than its rect (reuse uicheck's text-fit helper),
        "editor text fits at <theme>/<size>/<width>");
}
keyboard path, no mouse: Tab into the canvas; Space → quick-add; type "input", Enter; '/' focuses library search;
  arrows pan; F fits; i opens details; Esc closes; Tab reaches the inspector fields and "Delete node";
check(each step's expected effect, "every editor action works from the keyboard");
capture(panel, "blueprints-editor-dark"), (…"-light"), (…"-compact-dark"), quick-add and details captures in both themes.
```
- [ ] **Step 2: Run; fix every failure.** Light-theme token values may change here within Task 1's contrast checks.
- [ ] **Step 3: Capture references** with `QT_QPA_PLATFORM=offscreen MAGNITE_BLUEPRINTS_DESIGN_CHECK=1 ./build/magnite-app --ui-check <scratchpad>/captures` and with `--screenshot` in both themes; compare against `editor-v2.html` side by side and fix visible gaps (spacing, shadow, wire glow, header tint).
- [ ] **Step 4: Phase gate**, plus `MAGNITE_SCREENSHOT_THEME=dark` and light `--screenshot` runs succeed.
- [ ] **Step 5: Update the mockup README** to say the editor now ships this design and the mockup is historical.
- [ ] **Step 6: Commit** — `git commit -m "Polish the Blueprints editor for light theme, keyboard and compact sizes"`

---

## Not in this plan

- Merging `feature/blueprint-workflow-nodes` (owner's call, after Task 11).
- Undo/redo history (no history exists; no buttons shown).
- Redesigning the Blueprints library page, Create page, or the contents of Test cases and Run history.
