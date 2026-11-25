#include "testing/blueprint_editor_tests.h"

#include "panels/blueprintpreview.h"
#include "testing/blueprint_test_support.h"
#include "theme.h"
#include "widgets/blueprintscene.h"
#include "widgets/blueprintstyle.h"
#include "widgets/blueprintview.h"

#include <QContextMenuEvent>
#include <QWheelEvent>
#include <optional>

#include <QColor>
#include <QImage>
#include <QJsonArray>
#include <QPainter>
#include <QtMath>
#include <cstdio>

using namespace BlueprintTest;

namespace {
int g_failures = 0;

void check(bool condition, const char *message) {
    g_failures += !condition;
    fprintf(stderr, "[blueprint-editor] %s %s\n", condition ? "ok:" : "FAIL:", message);
}

qreal luminance(const QColor &c) {
    auto linear = [](qreal v) { return v <= 0.04045 ? v / 12.92 : qPow((v + 0.055) / 1.055, 2.4); };
    return 0.2126 * linear(c.redF()) + 0.7152 * linear(c.greenF()) + 0.0722 * linear(c.blueF());
}

qreal contrast(const QColor &a, const QColor &b) {
    const qreal x = luminance(a), y = luminance(b);
    return (qMax(x, y) + 0.05) / (qMin(x, y) + 0.05);
}

void checkStyle() {
    check(BlueprintStyle::typeFamily("integer") == "number" && BlueprintStyle::typeFamily("fields") == "object"
              && BlueprintStyle::typeFamily("adapter-ref") == "adapter" && BlueprintStyle::typeFamily("mystery") == "text",
          "port types map to the seven wire colours, unknown types as text");
    check(BlueprintStyle::typeFamily("index") == "number" && BlueprintStyle::typeFamily("order") == "object"
              && BlueprintStyle::typeFamily("array") == "array" && BlueprintStyle::typeFamily("receipt") == "receipt"
              && BlueprintStyle::typeFamily("boolean") == "boolean",
          "remaining port types map to their wire colours");
    check(BlueprintStyle::nodeFamily("dataset") == "sources" && BlueprintStyle::nodeFamily("reasoning") == "models"
              && BlueprintStyle::nodeFamily("tool") == "tools" && BlueprintStyle::nodeFamily("unheard-of") == "transform",
          "node kinds map to families, unknown kinds as Transform");
    check(BlueprintStyle::families() == QStringList({"sources", "transform", "knowledge", "models", "tools", "output"})
              && BlueprintStyle::familyLabel("models") == "Models" && BlueprintStyle::familyLabel("sources") == "Sources",
          "families are ordered and labelled");
    check(BlueprintStyle::nodeIcon("dataset") == "database" && BlueprintStyle::nodeIcon("unheard-of") == "cpu",
          "node icons keep the canvas mapping");
    for (bool dark : {true, false}) {
        Theme::setDark(dark);
        bool wires = true, distinct = true;
        const QStringList types = {"text", "number", "boolean", "object", "array", "receipt", "adapter-ref"};
        for (const auto &type : types) wires &= contrast(BlueprintStyle::typeColor(type), QColor(Theme::canvasBackground())) >= 3.0;
        for (const auto &family : BlueprintStyle::families()) distinct &= BlueprintStyle::familyColor(family).isValid();
        check(wires, dark ? "dark wire colours reach 3:1 on the canvas" : "light wire colours reach 3:1 on the canvas");
        check(distinct, "every family has a valid colour");
    }
}

void checkScene(const BlueprintPreview &preview) {
    BlueprintScene scene(preview);
    auto a = preview.makeNode("input", "a", 0, 0), b = preview.makeNode("output", "b", 300, 0);
    scene.setGraph({{"nodes", QJsonArray{a, b}}, {"edges", QJsonArray{edge("a", "text", "b", "value")}}});
    check(scene.nodes().size() == 2 && scene.wires().size() == 1, "the scene builds one item per node and edge");
    auto *w = scene.wires().first();
    check(qAbs(w->path().boundingRect().height()) <= 1.0, "a wire between aligned pins stays straight");
    check(w->pen().color() == BlueprintStyle::typeColor("text"), "wires take the colour of their data type");
    check(scene.node("b")->pin(false, "value")->anchor().x() < scene.node("b")->sceneBoundingRect().center().x()
              && scene.node("a")->pin(true, "text")->anchor().x() > scene.node("a")->sceneBoundingRect().center().x(),
          "inputs sit on the left edge and outputs on the right");
    scene.setGraph({{"nodes", QJsonArray{b}}});
    check(!scene.node("b")->warning().isEmpty(), "an unconnected input shows a warning mark");
    check(scene.node("b")->warning().contains(preview.portLabel(b["in"].toArray().first().toObject()) + " is not connected"),
          "the warning names the unconnected port");
    {
        auto bare = b;
        bare.remove("x");
        bare.remove("y");
        const QJsonObject loose{{"nodes", QJsonArray{a, bare}},
                                {"edges", QJsonArray{edge("a", "text", "b", "value"), edge("a", "text", "ghost", "value")}}};
        scene.setGraph(loose);
        check(scene.graph() == loose, "the scene returns the graph exactly as given, with dangling edges and no x/y");
        check(scene.node("b")->warning().isEmpty(), "a fully connected node has no warning");
        auto bad = preview.makeNode("parse-json", "p", 0, 200);
        const auto name = bad["name"].toString();
        bad["name"] = name;
        QString issueLine;
        for (const auto &line : preview.issues({{"nodes", QJsonArray{a, bad}}}))
            if (line.startsWith(name + " ")) issueLine = line;
        scene.setGraph({{"nodes", QJsonArray{a, bad}}});
        check(!issueLine.isEmpty() && scene.node("p")->warning().contains(issueLine),
              "a validation issue for a node appears in its warning");
    }
    scene.setGraph(preview.example());
    if (!scene.wires().isEmpty()) scene.wires().first()->setActive(true);
    scene.nodes().first()->setRunStatus("running");
    QImage frame(900, 600, QImage::Format_ARGB32_Premultiplied);
    frame.fill(Qt::black);
    QPainter painter(&frame);
    scene.render(&painter);
    painter.end();
    bool painted = false;
    for (int y = 0; y < frame.height() && !painted; ++y)
        for (int x = 0; x < frame.width() && !painted; ++x) painted = frame.pixel(x, y) != QColor(Qt::black).rgba();
    check(painted, "the scene paints nodes, pins and wires");
    check(scene.nodes().size() == preview.example()["nodes"].toArray().size(),
          "the legacy order example renders every node");
    auto odd = preview.makeNode("output", "odd", 0, 0);
    odd["kind"] = "unheard-of";
    odd["operation"] = "unheard-of";
    auto in = odd["in"].toArray();
    auto p = in[0].toObject();
    p["type"] = "mystery";
    in[0] = p;
    odd["in"] = in;
    scene.setGraph({{"nodes", QJsonArray{odd}}});
    check(scene.node("odd") && scene.node("odd")->pin(false, "value"), "unknown kinds and port types still render");
    check(scene.node("odd")->family() == "transform"
              && scene.node("odd")->boundingRect().isValid(),
          "an unknown kind renders as a Transform node");
}

void wheel(QWidget *w, QPointF at, int delta) {
    QWheelEvent event(at, w->mapToGlobal(at.toPoint()), {}, QPoint(0, delta), Qt::NoButton, Qt::NoModifier,
                      Qt::NoScrollPhase, false);
    QCoreApplication::sendEvent(w, &event);
}

void checkView(BlueprintPreview &preview) {
    BlueprintView view(preview);
    view.resize(900, 600);
    view.show();
    pump();
    const auto graph = preview.example();
    view.setGraph(graph);
    pump();
    check(view.graphScene()->graph() == graph && view.zoomPercent() > 0 && view.zoomPercent() <= 100,
          "the view fits a new graph on first show without rewriting it");
    view.fit();
    pump();
    const int fitted = view.zoomPercent();
    int emitted = 0;
    QObject::connect(&view, &BlueprintView::zoomChanged, [&](int) { ++emitted; });
    const QPointF centre(view.viewport()->width() / 2.0, view.viewport()->height() / 2.0);
    const QPointF cursor = centre + QPointF(150, 90);
    const QPointF underCursor = view.mapToScene(cursor.toPoint());
    wheel(view.viewport(), cursor, 120);
    check(view.zoomPercent() > fitted && emitted > 0, "wheel zooms the canvas");
    check((view.mapToScene(cursor.toPoint()) - underCursor).manhattanLength() < 2, "wheel zooms about the cursor");
    for (int i = 0; i < 40; ++i) wheel(view.viewport(), centre, 120);
    check(view.zoomPercent() == 250, "zoom stops at 2.5x");
    for (int i = 0; i < 80; ++i) wheel(view.viewport(), centre, -120);
    check(view.zoomPercent() == 25, "zoom out stops at 0.25x");
    view.fit();
    wheel(view.viewport(), centre, 120);
    // an empty point, found like the old fullscreen check
    const auto nodeBoxes = [&] {
        QList<QRectF> boxes;
        for (const auto &node : graph["nodes"].toArray())
            boxes << BlueprintScene::nodeRect(node.toObject()).adjusted(-16, -16, 16, 16);
        return boxes;
    }();
    std::optional<QPointF> empty;
    for (int y = 4; y < view.viewport()->height() && !empty; y += 12)
        for (int x = 4; x < view.viewport()->width(); x += 12) {
            const QPointF scene = view.mapToScene(QPoint(x, y));
            if (std::none_of(nodeBoxes.cbegin(), nodeBoxes.cend(), [&](const QRectF &r) { return r.contains(scene); })) {
                empty = QPointF(x, y);
                break;
            }
        }
    QHash<QString, int> moves;
    QObject::connect(&view, &BlueprintView::nodeMoved, [&](const QString &id, const QPoint &) { ++moves[id]; });
    const QPointF before = view.centerInGraph();
    check(empty.has_value(), "the example leaves empty canvas to drag");
    if (empty) drag(view.viewport(), *empty, *empty + QPointF(63, 29));
    check(view.centerInGraph() != before && moves.isEmpty(), "dragging the background pans without moving nodes");
    view.fit();
    pump();
    const QString first = graph["nodes"].toArray().first().toObject()["id"].toString();
    const auto centreOf = [&](const QString &id) {
        return viewPoint(view, view.graphScene()->node(id)->sceneBoundingRect().center());
    };
    QString picked;
    QObject::connect(&view, &BlueprintView::nodeSelected, [&](const QString &id) { picked = id; });
    const QPointF from = centreOf(first);
    const QPointF startPos = view.graphScene()->node(first)->pos();
    const QJsonObject savedGraph = view.graphScene()->graph();
    drag(view.viewport(), from, from + QPointF(40, 0), 30);
    check(moves.size() == 1 && moves.value(first) == 1, "dragging a node reports one move on release");
    check(view.graphScene()->node(first)->pos() != startPos && view.graphScene()->graph() == savedGraph,
          "the node moves in the scene while the stored graph stays untouched");
    check(picked == first && view.selection() == first, "pressing a node selects it");
    // a click without movement reports no move
    click(view.viewport(), centreOf(first));
    check(moves.value(first) == 1, "clicking a node without moving it reports no move");
    // setGraph keeps selection and camera, and does not refit after the user zoomed
    const QPointF cam = view.centerInGraph();
    const int zoomBefore = view.zoomPercent();
    view.setGraph(graph);
    check(view.selection() == first && view.zoomPercent() == zoomBefore && (view.centerInGraph() - cam).manhattanLength() < 1,
          "a rebuild keeps selection and camera once the user has moved the view");
    // setSelection is quiet; clearing by background click emits an empty id
    picked = "unset";
    view.setSelection(QString());
    check(view.selection().isEmpty() && picked == "unset", "setSelection changes the selection without a signal");
    view.setSelection(first);
    if (empty) click(view.viewport(), *empty);
    check(view.selection().isEmpty() && picked.isEmpty(), "clicking the background clears the selection");
    // keys
    view.setSelection("input");
    QString deleted, detailed;
    QObject::connect(&view, &BlueprintView::deleteRequested, [&](const QString &id) { deleted = id; });
    QObject::connect(&view, &BlueprintView::detailsRequested, [&](const QString &id) { detailed = id; });
    view.setSelection(first);
    key(view.viewport(), Qt::Key_Delete);
    check(deleted == first, "Delete asks to remove the selected node");
    deleted.clear();
    key(view.viewport(), Qt::Key_Backspace);
    check(deleted == first, "Backspace asks to remove the selected node");
    key(view.viewport(), Qt::Key_I, {}, "i");
    check(detailed == first, "I asks for the selected node's details");
    int adds = 0;
    QPointF addAt;
    QObject::connect(&view, &BlueprintView::quickAddRequested, [&](const QPoint &, const QPointF &g) { ++adds; addAt = g; });
    key(view.viewport(), Qt::Key_Space, {}, " ");
    key(view.viewport(), Qt::Key_Tab);
    check(adds == 2, "Space and Tab ask for quick add");
    if (empty) {
        QMouseEvent dbl(QEvent::MouseButtonDblClick, *empty, view.viewport()->mapToGlobal(empty->toPoint()),
                        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(view.viewport(), &dbl);
        check(adds == 3 && (addAt - view.mapToScene(empty->toPoint())).manhattanLength() < 1,
              "double-clicking the background asks for quick add at that point");
    }
    const int z = view.zoomPercent();
    key(view.viewport(), Qt::Key_Plus, {}, "+");
    check(view.zoomPercent() > z, "+ zooms in");
    key(view.viewport(), Qt::Key_Minus, {}, "-");
    check(view.zoomPercent() == z || qAbs(view.zoomPercent() - z) <= 1, "- zooms out");
    const QPointF panFrom = view.centerInGraph();
    key(view.viewport(), Qt::Key_Left);
    check(view.centerInGraph().x() < panFrom.x(), "arrow keys pan");
    key(view.viewport(), Qt::Key_F);
    check(view.zoomPercent() == fitted, "F fits the graph");
    key(view.viewport(), Qt::Key_Right);
    key(view.viewport(), Qt::Key_Home);
    check(view.zoomPercent() == fitted, "Home fits the graph");
    // menus
    QString menuNode;
    int canvasMenus = 0;
    QObject::connect(&view, &BlueprintView::nodeMenuRequested, [&](const QString &id, const QPoint &, const QPointF &) { menuNode = id; });
    QObject::connect(&view, &BlueprintView::canvasMenuRequested, [&](const QPoint &, const QPointF &) { ++canvasMenus; });
    const QPoint onNode = centreOf(first).toPoint();
    QContextMenuEvent menu(QContextMenuEvent::Mouse, onNode, view.viewport()->mapToGlobal(onNode));
    QCoreApplication::sendEvent(view.viewport(), &menu);
    check(menuNode == first && view.selection() == first, "right-clicking a node opens its menu and selects it");
    if (empty) {
        QContextMenuEvent bg(QContextMenuEvent::Mouse, empty->toPoint(), view.viewport()->mapToGlobal(empty->toPoint()));
        QCoreApplication::sendEvent(view.viewport(), &bg);
        check(canvasMenus == 1, "right-clicking the background opens the canvas menu");
    }
    // run states
    view.setRunStates({{first, "running"}}, first);
    check(view.graphScene()->node(first)->runStatus() == "running", "run states reach the nodes");
    view.setGraph(graph);
    check(view.graphScene()->node(first)->runStatus() == "running", "run states survive a rebuild");
    view.clearRunStates();
    check(view.graphScene()->node(first)->runStatus().isEmpty(), "clearing run states resets the nodes");
    check(view.accessibleName() == "Workflow graph" && view.accessibleDescription().contains("Delete"),
          "the canvas has an accessible name and description");
    // background dots
    view.fit();
    pump();
    const QImage shot = view.viewport()->grab().toImage();
    bool dots = false;
    for (int y = 0; y < 30 && !dots; ++y)
        for (int x = 0; x < 30 && !dots; ++x) dots = QColor(shot.pixel(x, y)) != QColor(Theme::canvasBackground());
    check(dots, "canvas dots stay visible");
    // performance
    QJsonArray nodes, edges;
    for (int i = 0; i < 40; ++i)
        nodes << preview.makeNode(i % 2 ? "count" : "input", QString("n%1").arg(i), (i % 8) * 260, (i / 8) * 160);
    for (int i = 0; i < 60; ++i) edges << edge(QString("n%1").arg(i % 40), "value", QString("n%1").arg((i + 1) % 40), "value");
    view.setGraph({{"nodes", nodes}, {"edges", edges}});
    QElapsedTimer timer;
    timer.start();
    view.fit();
    for (int k = 0; k < 60; ++k) view.viewport()->repaint();
    check(timer.elapsed() < 1500, "a 40-node graph repaints 60 times in under 1.5 s");
}
}  // namespace

int runBlueprintEditorChecks(const QString &captures) {
    Q_UNUSED(captures);
    g_failures = 0;
    const bool original = Theme::isDark();
    Theme::setDark(true);
    BlueprintPreview preview(nullptr, false);
    checkStyle();
    checkScene(preview);
    checkView(preview);
    Theme::setDark(original);
    fprintf(stderr, "[blueprint-editor] %s (%d failures)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures;
}
