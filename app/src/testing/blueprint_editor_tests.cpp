#include "testing/blueprint_editor_tests.h"

#include "panels/blueprintbindingfields.h"
#include "panels/blueprintdetails.h"
#include "panels/blueprintinspector.h"
#include "panels/blueprintlibrarydock.h"
#include "panels/blueprintnodefields.h"
#include "panels/blueprintpanel.h"
#include "panels/blueprintquickadd.h"
#include "panels/blueprintpreview.h"
#include "panels/blueprinttopbar.h"
#include "testing/blueprint_test_support.h"
#include "testing/textfit.h"
#include "theme.h"
#include "widgets/blueprintminimap.h"
#include "widgets/blueprintscene.h"
#include "widgets/blueprintstyle.h"
#include "widgets/blueprintview.h"
#include "widgets/floatingpanel.h"

#include <QContextMenuEvent>
#include <QDropEvent>
#include <QListWidget>
#include <QMimeData>
#include <QShortcut>
#include <QCursor>
#include <QDir>
#include <QSet>
#include <QSettings>
#include <QMenu>
#include <QRegularExpression>
#include <QTabWidget>
#include <QLineEdit>
#include <QComboBox>
#include <QPlainTextEdit>
#include <QVBoxLayout>
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
    key(&view, Qt::Key_Tab);  // the focused widget is the view; the dock's buttons make the viewport path a focus move
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
    // single selection: Ctrl-click never extends it
    {
        const auto ids = graph["nodes"].toArray();
        const QString a = ids.at(0).toObject()["id"].toString(), b = ids.at(1).toObject()["id"].toString();
        view.fit();
        pump();
        QStringList seenIds;
        const auto conn = QObject::connect(&view, &BlueprintView::nodeSelected, [&](const QString &id) { seenIds << id; });
        click(view.viewport(), centreOf(a), Qt::ControlModifier);
        click(view.viewport(), centreOf(b), Qt::ControlModifier);
        int selectedCount = 0;
        for (auto *node : view.graphScene()->nodes()) selectedCount += node->isSelected();
        check(selectedCount == 1 && view.selection() == b && view.graphScene()->node(b)->isSelected()
                  && !seenIds.isEmpty() && seenIds.last() == b,
              "Ctrl-clicking a second node selects only that node");
        QObject::disconnect(conn);
        deleted.clear();
        key(view.viewport(), Qt::Key_Delete);
        check(deleted == b, "Delete acts on the single selected node");
        key(view.viewport(), Qt::Key_Delete, Qt::ControlModifier);
        key(view.viewport(), Qt::Key_I, Qt::AltModifier, "i");
        key(view.viewport(), Qt::Key_Left, Qt::ControlModifier);
        check(deleted == b && detailed == first, "bare-key shortcuts ignore Ctrl and Alt combinations");
    }
    // focus keys leave the canvas instead of quick add
    {
        QWidget host;
        auto *layout = new QVBoxLayout(&host);
        auto *before = new QLineEdit, *after = new QLineEdit;
        BlueprintView inner(preview);
        layout->addWidget(before);
        layout->addWidget(&inner);
        layout->addWidget(after);
        host.resize(500, 600);
        host.show();
        pump();
        int innerAdds = 0;
        QObject::connect(&inner, &BlueprintView::quickAddRequested, [&](const QPoint &, const QPointF &) { ++innerAdds; });
        inner.setFocus();
        key(&inner, Qt::Key_Tab, Qt::ControlModifier);
        const bool inDock = host.focusWidget() && host.focusWidget() != &inner && inner.isAncestorOf(host.focusWidget());
        check(innerAdds == 0 && inDock, "Ctrl+Tab moves focus forward off the canvas into the dock");
        for (int i = 0; i < 12 && host.focusWidget() && host.focusWidget() != after; ++i) key(host.focusWidget(), Qt::Key_Tab);
        check(host.focusWidget() == after, "the dock leads on to the next widget");
        inner.setFocus();
        key(&inner, Qt::Key_Backtab, Qt::ShiftModifier);
        check(innerAdds == 0 && host.focusWidget() == before, "Shift+Tab moves focus back out of the canvas");
        check(inner.accessibleDescription().contains("Control+Tab") && inner.accessibleDescription().contains("Shift+Tab"),
              "the accessible description names the focus keys");
    }
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

QJsonObject findPort(const QJsonObject &graph, const QString &node, const QString &side, const QString &port) {
    for (const auto &n : graph["nodes"].toArray())
        if (n.toObject()["id"] == node)
            for (const auto &p : n.toObject()[side].toArray())
                if (p.toObject()["id"] == port) return p.toObject();
    return {};
}

void checkConnecting(BlueprintPreview &preview) {
    auto in = preview.makeNode("input", "in", 0, 0), cnt = preview.makeNode("count", "cnt", 0, 200),
         out = preview.makeNode("output", "out", 320, 0);
    QJsonObject g{{"nodes", QJsonArray{in, cnt, out}}, {"edges", QJsonArray{edge("in", "text", "out", "value")}}};
    check(preview.dropError(g, edge("in", "text", "cnt", "value")).contains("Text cannot connect to Array"),
          "dropping on an incompatible pin explains why with the existing text");
    check(preview.choosesType(out, "in", "value") && !preview.choosesType(cnt, "in", "value")
              && preview.choosesType(in, "out", "text"),
          "chooseType is read from the operation spec");
    int dropped = -1;
    auto r = preview.connectGraph(g, edge("cnt", "value", "out", "value"), &dropped);
    check(r["edges"].toArray().size() == 1 && r["edges"].toArray().first().toObject()["from"] == "cnt",
          "dropping on an occupied input replaces its wire");
    check(findPort(r, "out", "in", "value")["type"] == "integer" && dropped == 0,
          "a port that chooses its type adopts the wire's type");
    check(findPort(r, "out", "in", "value")["schema"].toObject()["type"] == "integer" && g["nodes"].toArray().size() == 3
              && findPort(g, "out", "in", "value")["type"] == "text",
          "the adopted port carries the contract schema and the input graph is untouched");
    // the target adopts the source type, and wires the retyped node can no longer hold are dropped
    auto parse = preview.makeNode("parse-json", "pj", 0, 400);
    QJsonObject h{{"nodes", QJsonArray{in, out, parse}},
                  {"edges", QJsonArray{edge("in", "text", "pj", "value"), edge("pj", "value", "out", "value")}}};
    dropped = -1;
    r = preview.connectGraph(h, edge("pj", "value", "out", "value"), &dropped);
    check(!r.isEmpty() && dropped == 0 && findPort(r, "out", "in", "value")["type"] == "object",
          "adopting a type keeps wires that stay valid");
    QJsonObject cycle{{"nodes", QJsonArray{in, cnt, out}}, {"edges", QJsonArray{}}};
    QString error;
    r = preview.connectGraph(cycle, edge("cnt", "value", "cnt", "value"), nullptr, &error);
    check(r.isEmpty() && !error.isEmpty(), "an invalid connection returns an empty graph and the reason");

    // free chooseType source adopts the target's type
    QJsonObject free{{"nodes", QJsonArray{in, cnt, out}}, {"edges", QJsonArray{}}};
    check(preview.dropError(free, edge("in", "text", "cnt", "value")).isEmpty(), "an unwired chooseType output may drop on a typed input");
    r = preview.connectGraph(free, edge("in", "text", "cnt", "value"));
    check(findPort(r, "in", "out", "text")["type"] == "array" && findPort(r, "in", "out", "text")["schema"].toObject()["type"] == "array",
          "a free output that chooses its type adopts the target's type");
    // retyping prunes other edges on the node that stop being valid
    auto cnt2 = preview.makeNode("count", "cnt2", 640, 0);
    QJsonObject stale{{"nodes", QJsonArray{in, cnt, out, cnt2}}, {"edges", QJsonArray{edge("out", "value", "cnt2", "value")}}};
    dropped = -1;
    r = preview.connectGraph(stale, edge("cnt", "value", "out", "value"), &dropped);
    check(dropped == 1 && r["edges"].toArray().size() == 1 && r["edges"].toArray().first().toObject()["from"] == "cnt",
          "retyping drops other edges on the node that become invalid and counts them");
    {
        const QPointF from(600, 0), to(0, 300);
        const QRectF bounds = WireItem::pathBetween(from, to).boundingRect();
        check(bounds.left() >= to.x() - 130 && bounds.right() <= from.x() + 130,
              "a backward wire stays within 130px beyond its endpoints");
        const auto forward = WireItem::pathBetween(QPointF(0, 0), QPointF(600, 300));
        const auto straight = WireItem::pathBetween(QPointF(0, 40), QPointF(300, 40));
        check(forward.elementAt(1).x == 300 && straight.elementCount() == 2,
              "forward wires keep their reach and aligned wires stay straight");
    }
    // (no fixture node can reach this from valid graphs: only a chooseType input retypes, and its sole edge is the replaced one)
    BlueprintView view(preview);
    view.resize(900, 600);
    view.show();
    view.setDropValidator([&](const QJsonObject &e) { return preview.dropError(g, e); });
    view.setGraph(g);
    pump();
    auto *viewport = view.viewport();
    const auto pinPoint = [&](const char *node, bool output, const char *port) {
        return viewPoint(view, view.graphScene()->node(node)->pin(output, port)->anchor());
    };
    QJsonObject asked;
    QObject::connect(&view, &BlueprintView::connectRequested, [&](const QJsonObject &e) { asked = e; });
    drag(viewport, pinPoint("cnt", true, "value"), pinPoint("out", false, "value"));
    check(asked == edge("cnt", "value", "out", "value"), "dragging from an output to an input asks to connect");
    asked = {};
    drag(viewport, pinPoint("in", true, "text"), pinPoint("cnt", false, "value"));
    check(asked.isEmpty() && visibleText(view, "cannot connect"),
          "an incompatible drop connects nothing and shows the reason");
    bool named = false;
    for (auto *label : view.findChildren<QLabel *>()) named |= label->isVisible() && label->objectName() == "blueprintDropMessage";
    check(named, "the reason is shown in the themed drop message label");
    QPointF emptyPoint = viewPoint(view, QPointF(160, 150));
    check(view.items(emptyPoint.toPoint()).isEmpty(), "the empty drop point is empty canvas");
    bool canvasDrop = false;
    QObject::connect(&view, &BlueprintView::wireDropped,
                     [&](const QString &from, const QString &, const QPoint &, const QPointF &) { canvasDrop = from == "cnt"; });
    drag(viewport, pinPoint("cnt", true, "value"), emptyPoint);
    check(canvasDrop, "dropping a wire on empty canvas is reported");
    // highlights while dragging
    {
        QMouseEvent press(QEvent::MouseButtonPress, pinPoint("in", true, "text"), viewport->mapToGlobal(pinPoint("in", true, "text").toPoint()),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(viewport, &press);
        const auto *countIn = view.graphScene()->node("cnt")->pin(false, "value");
        const auto *outIn = view.graphScene()->node("out")->pin(false, "value");
        check(countIn->highlight() == PinItem::Highlight::Dimmed && outIn->highlight() == PinItem::Highlight::Compatible,
              "pressing an output highlights compatible inputs and dims the others");
        QMouseEvent release(QEvent::MouseButtonRelease, emptyPoint, viewport->mapToGlobal(emptyPoint.toPoint()),
                            Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(viewport, &release);
        check(countIn->highlight() == PinItem::Highlight::None && outIn->highlight() == PinItem::Highlight::None,
              "releasing clears the pin highlights");
    }
    // picking up a wired input
    QJsonObject removedByDrag, reconnected;
    QObject::connect(&view, &BlueprintView::disconnectRequested, [&](const QJsonObject &e) { removedByDrag = e; });
    QObject::connect(&view, &BlueprintView::connectRequested, [&](const QJsonObject &e) { reconnected = e; });
    drag(viewport, pinPoint("out", false, "value"), emptyPoint);
    check(removedByDrag == edge("in", "text", "out", "value") && reconnected.isEmpty(),
          "dragging a wire off its input and dropping on empty canvas disconnects it");
    removedByDrag = {};
    drag(viewport, pinPoint("out", false, "value"), pinPoint("out", false, "value"));
    check(removedByDrag.isEmpty() && reconnected.isEmpty(), "dropping a picked-up wire back on its own input changes nothing");
    check(view.graphScene()->wires().first()->isVisible(), "a cancelled pick-up leaves the wire visible");
    // Esc cancels a drag
    {
        const int itemsBefore = view.graphScene()->items().size();
        asked = {};
        canvasDrop = false;
        const QPointF from = pinPoint("cnt", true, "value");
        QMouseEvent press(QEvent::MouseButtonPress, from, viewport->mapToGlobal(from.toPoint()), Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(viewport, &press);
        check(view.graphScene()->items().size() == itemsBefore + 1, "a wire drag adds a temporary wire");
        key(viewport, Qt::Key_Escape);
        const auto *outIn = view.graphScene()->node("out")->pin(false, "value");
        check(view.graphScene()->items().size() == itemsBefore && outIn->highlight() == PinItem::Highlight::None,
              "Escape removes the temporary wire and clears the highlights");
        QMouseEvent release(QEvent::MouseButtonRelease, emptyPoint, viewport->mapToGlobal(emptyPoint.toPoint()), Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        QCoreApplication::sendEvent(viewport, &release);
        check(asked.isEmpty() && !canvasDrop, "a cancelled drag emits nothing");
    }
    // moving a picked-up wire to another valid input
    {
        auto out2 = preview.makeNode("output", "out2", 320, 200);
        QJsonObject g2{{"nodes", QJsonArray{in, out, out2}}, {"edges", QJsonArray{edge("in", "text", "out", "value")}}};
        view.setDropValidator([&](const QJsonObject &e) { return preview.dropError(g2, e); });
        view.setGraph(g2);
        pump();
        QStringList order;
        QJsonObject gone, added, moving;
        QObject::disconnect(&view, &BlueprintView::disconnectRequested, nullptr, nullptr);
        QObject::disconnect(&view, &BlueprintView::connectRequested, nullptr, nullptr);
        QObject::connect(&view, &BlueprintView::disconnectRequested, [&](const QJsonObject &) { order << "disconnect"; });
        QObject::connect(&view, &BlueprintView::connectRequested, [&](const QJsonObject &) { order << "connect"; });
        QObject::connect(&view, &BlueprintView::wireMoved, [&](const QJsonObject &from, const QJsonObject &to) {
            order << "move";
            gone = from;
            added = to;
        });
        view.setDropValidator([&](const QJsonObject &e) {
            moving = view.movingEdge();
            return preview.dropError(g2, e);
        });
        drag(viewport, pinPoint("out", false, "value"), pinPoint("out2", false, "value"));
        check(order == QStringList({"move"}) && gone == edge("in", "text", "out", "value")
                  && added == edge("in", "text", "out2", "value"),
              "moving a picked-up wire to another input is one wireMoved(old, new), not a disconnect and connect");
        check(moving == edge("in", "text", "out", "value") && view.movingEdge().isEmpty(),
              "validators see the picked-up edge while it moves, and nothing after the drop");
        QObject::disconnect(&view, &BlueprintView::wireMoved, nullptr, nullptr);
        view.setDropValidator([&](const QJsonObject &e) { return preview.dropError(g, e); });
        view.setGraph(g);
        pump();
    }
    // wire selection and removal
    QStringList selected;
    QObject::connect(&view, &BlueprintView::nodeSelected, [&](const QString &id) { selected << id; });
    view.setSelection("cnt");
    selected.clear();
    QJsonObject removed;
    QObject::disconnect(&view, &BlueprintView::disconnectRequested, nullptr, nullptr);
    QObject::connect(&view, &BlueprintView::disconnectRequested, [&](const QJsonObject &e) { removed = e; });
    int deletes = 0;
    QObject::connect(&view, &BlueprintView::deleteRequested, [&](const QString &) { ++deletes; });
    click(viewport, viewPoint(view, view.graphScene()->wires().first()->path().pointAtPercent(0.5)));
    check(selected == QStringList{""} && view.selection().isEmpty(), "selecting a wire clears the node selection");
    key(viewport, Qt::Key_Delete);
    check(removed == edge("in", "text", "out", "value") && deletes == 0, "a selected wire is removed with Delete");
    click(viewport, viewPoint(view, view.graphScene()->node("cnt")->pos() + QPointF(40, 12)));
    removed = {};
    key(viewport, Qt::Key_Delete);
    check(view.selection() == "cnt" && removed.isEmpty() && deletes == 1, "selecting a node deselects the wire and Delete removes the node");
}
bool hasEdge(const QJsonObject &graph, const QJsonObject &wanted) {
    for (const auto &value : graph["edges"].toArray())
        if (value.toObject() == wanted) return true;
    return false;
}

void checkPanelCanvas(BlueprintPreview &preview) {
    BlueprintPanel panel(&preview);
    panel.resize(1120, 720);
    panel.show();
    pump();
    const auto example = preview.example();
    const auto id = example["id"].toString();
    preview.saveBlueprint(example);
    pump();
    for (auto *button : panel.findChildren<QPushButton *>()) {
        if (button->isVisible() && button->text() == "Open" && visibleText(*button->parentWidget(), "Order intake")) {
            button->click();
            break;
        }
    }
    pump(100);
    const auto before = preview.saved();
    auto *view = panel.findChild<BlueprintView *>();
    check(view && view->isVisible(), "the editor shows the graphics canvas");
    if (!view || !view->isVisible()) return;
    view->fit();
    const QPointF centre(view->viewport()->width() / 2.0, view->viewport()->height() / 2.0);
    wheel(view->viewport(), centre, 120);
    std::optional<QPointF> empty;
    for (int y = 4; y < view->viewport()->height() && !empty; y += 12)
        for (int x = 4; x < view->viewport()->width(); x += 12)
            if (view->items(QPoint(x, y)).isEmpty()) {
                empty = QPointF(x, y);
                break;
            }
    if (empty) drag(view->viewport(), *empty, *empty + QPointF(40, 20));
    pump(200);
    check(empty && preview.saved() == before, "opening, fitting, zooming and panning never re-save the graph");
    view->fit();
    pump();
    const auto fitted = view->centerInGraph();
    view->revealNode(example["nodes"].toArray().first().toObject()["id"].toString());
    check(view->centerInGraph() == fitted, "revealing a node that is already in view keeps the camera");

    // Saved layouts space nodes 219px apart; at the node width no input pin may sit within reach of another
    // node's output pin, or pressing the output would pick up the input's wire instead.
    view->fit();
    pump();
    bool apart = true;
    for (auto *a : view->graphScene()->nodes())
        for (auto *b : view->graphScene()->nodes())
            for (auto *out : a->pins())
                for (auto *in : b->pins())
                    if (a != b && out->isOutput() && !in->isOutput())
                        apart &= QLineF(out->anchor(), in->anchor()).length() >= BlueprintStyle::pinHitRadius;
    check(apart, "no input pin in the example lies within the pin hit radius of another node's output pin");
    // An output that can replace an existing wire on its (occupied) input without dropping other wires,
    // at view points where each pin is the top-most item.
    const auto hitPoint = [&](const QString &node, bool output, const QString &port) -> std::optional<QPointF> {
        auto *item = view->graphScene()->node(node);
        auto *pin = item ? item->pin(output, port) : nullptr;
        if (!pin) return std::nullopt;
        const QPointF anchor = viewPoint(*view, pin->anchor());
        for (auto *hit : view->items(anchor.toPoint()))
            if (auto *top = dynamic_cast<PinItem *>(hit)) return top == pin ? std::optional(anchor) : std::nullopt;
        return std::nullopt;
    };
    const auto graph = preview.blueprint(id);
    QJsonObject replaced, added;
    std::optional<QPointF> from, to;
    for (const auto &edgeValue : graph["edges"].toArray()) {
        const auto existing = edgeValue.toObject();
        for (const auto &nodeValue : graph["nodes"].toArray()) {
            const auto node = nodeValue.toObject();
            for (const auto &portValue : node["out"].toArray()) {
                const auto candidate = edge(node["id"].toString(), portValue.toObject()["id"].toString(),
                                            existing["to"].toString(), existing["in"].toString());
                if (candidate["from"] == existing["from"] && candidate["out"] == existing["out"]) continue;
                int dropped = -1;
                if (preview.connectGraph(graph, candidate, &dropped).isEmpty() || dropped != 0) continue;
                from = hitPoint(candidate["from"].toString(), true, candidate["out"].toString());
                to = hitPoint(candidate["to"].toString(), false, candidate["in"].toString());
                if (!from || !to) continue;
                replaced = existing;
                added = candidate;
                break;
            }
            if (!added.isEmpty()) break;
        }
        if (!added.isEmpty()) break;
    }
    check(!added.isEmpty(), "the example has an occupied input another output can replace");
    if (added.isEmpty()) return;
    drag(view->viewport(), *from, *to);
    pump(50);
    const auto connected = preview.blueprint(id);
    check(hasEdge(connected, added) && !hasEdge(connected, replaced)
              && connected["edges"].toArray().size() == graph["edges"].toArray().size(),
          "dropping on an occupied input replaces its wire in the saved graph");

    WireItem *wire = nullptr;
    for (auto *item : view->graphScene()->wires())
        if (item->edge() == added) wire = item;
    check(wire, "the new wire is on the canvas");
    if (!wire) return;
    click(view->viewport(), viewPoint(*view, wire->path().pointAtPercent(0.5)));
    key(view->viewport(), Qt::Key_Delete);
    pump(50);
    const auto removed = preview.blueprint(id);
    check(!hasEdge(removed, added) && removed["edges"].toArray().size() == graph["edges"].toArray().size() - 1,
          "Delete removes the selected wire from the saved graph");
    preview.deleteBlueprint(id);

    // Moving a wire is one edit: one save, old edge replaced, and a declined prompt stores nothing.
    const auto openNamed = [&](const QJsonObject &blueprint) -> BlueprintView * {
        preview.saveBlueprint(blueprint);
        if (auto *library = visibleNamed<QPushButton>(panel, "Back to library")) library->click();
        pump();
        for (auto *button : panel.findChildren<QPushButton *>())
            if (button->isVisible() && button->text() == "Open"
                    && visibleText(*button->parentWidget(), blueprint["name"].toString())) {
                button->click();
                break;
            }
        pump(100);
        auto *opened = panel.findChild<BlueprintView *>();
        if (!opened || !opened->graphScene()->node("o2")) return nullptr;
        opened->fit();
        pump();
        return opened;
    };
    const auto pinAt = [](BlueprintView *v, const QString &node, bool output, const QString &port) {
        return viewPoint(*v, v->graphScene()->node(node)->pin(output, port)->anchor());
    };
    int saves = 0;
    QObject::connect(&preview, &BlueprintPreview::savedChanged, &panel, [&] { ++saves; });
    const auto cnt = preview.makeNode("count", "cnt", 0, 0), o2 = preview.makeNode("output", "o2", 320, 0),
               out = preview.makeNode("output", "out", 320, 200);
    auto moving = preview.connectGraph(QJsonObject{{"id", "move-check"}, {"name", "Move check"}, {"revision", 1},
                                                   {"nodes", QJsonArray{cnt, o2, out}}, {"edges", QJsonArray{}}},
                                       edge("cnt", "value", "o2", "value"));
    if (auto *v = openNamed(moving)) {
        saves = 0;
        drag(v->viewport(), pinAt(v, "o2", false, "value"), pinAt(v, "out", false, "value"));
        pump(50);
        const auto stored = preview.blueprint("move-check");
        check(saves == 1 && stored["edges"].toArray() == QJsonArray{edge("cnt", "value", "out", "value")},
              "moving a wire to another input saves once and replaces the old edge");
    } else check(false, "the move check graph opens in the editor");
    preview.deleteBlueprint("move-check");

    // No valid fixture graph makes a move drop wires (retyping touches only the dropped-on input), so the
    // prompt is reached through a stored graph with a stale, already incompatible edge on that input's node.
    auto stale = moving;
    stale["id"] = "move-decline";
    stale["name"] = "Move decline";
    auto staleNodes = stale["nodes"].toArray();
    staleNodes.append(preview.makeNode("count", "cnt2", 640, 200));
    stale["nodes"] = staleNodes;
    auto staleEdges = stale["edges"].toArray();
    staleEdges.append(edge("out", "value", "cnt2", "value"));
    stale["edges"] = staleEdges;
    int wouldDrop = 0;
    preview.connectGraph(QJsonObject{{"nodes", staleNodes}, {"edges", QJsonArray{staleEdges.at(1)}}},
                         edge("cnt", "value", "out", "value"), &wouldDrop);
    check(wouldDrop == 1, "the stale graph's move would drop one connection");
    if (auto *v = openNamed(stale)) {
        const auto storedBefore = preview.blueprint("move-decline");
        saves = 0;
        bool prompted = false;
        QTimer::singleShot(0, &panel, [&] {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                prompted = visibleText(*dialog, "This change will remove 1 incompatible connection.");
                dialog->reject();
            }
        });
        drag(v->viewport(), pinAt(v, "o2", false, "value"), pinAt(v, "out", false, "value"));
        pump(50);
        check(prompted && saves == 0 && preview.blueprint("move-decline") == storedBefore,
              "declining the remove-connections prompt for a moved wire leaves the stored graph unchanged");
        check(v->graphScene()->wires().size() == 2, "after a declined move the canvas shows the stored wires again");
    } else check(false, "the decline check graph opens in the editor");
    preview.deleteBlueprint("move-decline");
}

void checkOverlays(BlueprintPreview &preview) {
    BlueprintView view(preview);
    view.resize(900, 600);
    view.show();
    view.setGraph(preview.example());
    pump(50);
    const QStringList names = {"Add node", "Zoom in", "Zoom out", "Fit graph", "Show node library"};
    bool all = true;
    for (const auto &name : names) all &= visibleButton(view, name) != nullptr;
    check(all, "the bottom dock carries add, zoom and fit");
    check(!visibleButton(view, "Undo") && !visibleButton(view, "Redo"), "the dock has no undo or redo buttons");

    auto *mini = view.findChild<BlueprintMinimap *>();
    const QRect viewport = view.viewport()->geometry();
    check(mini && mini->isVisible() && mini->size() == QSize(180, 112)
              && (viewport.bottomRight() - mini->geometry().bottomRight()).manhattanLength() <= 32
              && viewport.right() - mini->geometry().right() <= 16 && viewport.bottom() - mini->geometry().bottom() <= 16,
          "the minimap sits in the bottom-right corner");
    FloatingPanel *dock = nullptr;
    for (auto *panel : view.findChildren<FloatingPanel *>())
        if (panel->isVisible()) dock = panel;
    const int shadow = FloatingPanel::shadowMargin();
    check(dock && qAbs(dock->geometry().center().x() - viewport.center().x()) <= 1
              && qAbs(viewport.bottom() - dock->geometry().adjusted(shadow, shadow, -shadow, -shadow).bottom() - 12) <= 1,
          "the dock is centred along the bottom edge");

    const auto c0 = view.centerInGraph();
    if (mini) click(mini, QPointF(10, 10));
    check(view.centerInGraph() != c0, "clicking the minimap moves the view");
    const int before = view.zoomPercent();
    if (auto *in = visibleButton(view, "Zoom in")) in->click();
    check(view.zoomPercent() > before, "Zoom in zooms");
    if (auto *out = visibleButton(view, "Zoom out")) out->click();
    check(view.zoomPercent() == before, "Zoom out undoes it");
    auto *label = visibleNamed<QLabel>(view, "Zoom level");
    check(label && label->text() == QString("%1%").arg(view.zoomPercent()), "the dock shows the zoom percentage");

    bool toggled = false, added = false;
    QObject::connect(&view, &BlueprintView::libraryToggled, [&](bool on) { toggled = on; });
    QObject::connect(&view, &BlueprintView::addNodeClicked, [&] { added = true; });
    if (auto *library = visibleButton(view, "Show node library")) library->click();
    if (auto *add = visibleButton(view, "Add node")) add->click();
    check(toggled && added, "the library toggle and Add node report to the view's signals");

    // No border: the body's edge pixels are the panel fill, like the pixels just inside them.
    if (dock) {
        const QImage image = dock->grab().toImage();
        const int m = FloatingPanel::shadowMargin();
        const QColor fill(Theme::blueprintPanelFill());
        const int cx = image.width() / 2, cy = image.height() / 2;
        const QPoint topEdge(cx, m), topInside(cx, m + 3), leftEdge(m, cy), leftInside(m + 3, cy);
        const QPoint topOut(cx, m - 1), leftOut(m - 1, cy);
        const QColor border(Theme::surfaceBorder());
        check(image.pixelColor(topEdge) == fill && image.pixelColor(topInside) == fill
                  && image.pixelColor(leftEdge) == fill && image.pixelColor(leftInside) == fill,
              "a floating panel's edge pixels equal its fill");
        check(image.pixelColor(topOut) != border && image.pixelColor(leftOut) != border,
              "floating panels have no border line");
        const int pad = dock->contentsMargins().top() + dock->contentsMargins().bottom();
        check(dock->height() <= 32 + pad + 4, "the dock is not padded twice");
    }

    // The free area excludes visible side overlays and follows their visibility.
    auto *side = new QWidget;
    side->setFixedWidth(200);
    view.addOverlay(side, OverlayEdge::Left);
    pump(30);
    check(view.freeArea().left() >= 200 && view.freeArea().right() == view.viewport()->geometry().right(),
          "freeArea excludes a visible left overlay");
    side->hide();
    pump(30);
    check(view.freeArea() == view.viewport()->geometry(), "freeArea ignores a hidden overlay");
    view.resize(700, 500);
    pump(50);
    check(mini && view.viewport()->geometry().right() - mini->geometry().right() <= 16, "overlays follow a resize");
}
// The texts of the menu a button opens (the popup closes again).
QStringList menuTexts(QPushButton *button) {
    QStringList texts;
    if (!button) return texts;
    QTimer::singleShot(0, button, [&texts] {
        if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
            for (const auto *action : menu->actions())
                if (action->isVisible() && !action->isSeparator()) texts << action->text();
            menu->close();
        }
    });
    button->click();
    pump();
    return texts;
}

void checkShell(BlueprintPreview &preview, const QString &captures) {
    Theme::setDark(true);  // the captures are of the dark editor
    BlueprintPanel panel(&preview);
    panel.resize(1120, 720);
    panel.show();
    pump();
    const auto example = preview.example();
    preview.saveBlueprint(example);
    pump();
    for (auto *button : panel.findChildren<QPushButton *>())
        if (button->isVisible() && button->text() == "Open" && visibleText(*button->parentWidget(), "Order intake")) {
            button->click();
            break;
        }
    pump(100);
    auto *view = panel.findChild<BlueprintView *>();
    if (!view || !view->isVisible()) {
        check(false, "the shell check opens the example");
        return;
    }
    bool tabs = false;
    for (auto *tab : panel.findChildren<QTabWidget *>()) tabs |= tab->isVisible();
    check(!tabs && visibleButton(panel, "Build") && visibleButton(panel, "Test cases") && visibleButton(panel, "Run history")
              && visibleButton(panel, "Run"),
          "a segmented control replaces the editor tabs");
    auto *bar = panel.findChild<QWidget *>("blueprintTopBar");
    auto *validation = bar ? bar->findChild<QPushButton *>("blueprintValidation") : nullptr;
    const QRegularExpression issueCount("^\\d+ issues?$");
    check(bar && bar->isVisible() && visibleText(*bar, "Order intake") && validation && validation->isVisible()
              && (validation->accessibleName() == "Ready to run" || issueCount.match(validation->text()).hasMatch()),
          "the top bar shows the name and live validation");
    check(bar && view->isAncestorOf(bar), "in Build the top bar floats over the canvas");
    auto *inspector = panel.findChild<FloatingPanel *>("blueprintInspector");
    check(inspector && !inspector->isVisible(), "the inspector is hidden with nothing selected");

    const auto select = [&](const QString &id) {
        auto *item = view->graphScene()->node(id);
        if (item) click(view->viewport(), viewPoint(*view, item->sceneBoundingRect().center()));
        pump(50);
    };
    const auto first = example["nodes"].toArray().first().toObject()["id"].toString();
    select(first);
    const QRect viewport = view->viewport()->geometry();
    check(view->selection() == first && inspector && inspector->isVisible()
              && qAbs(viewport.right() - inspector->geometry().right()) <= 16,
          "selecting a node floats the inspector in on the right");
    check(inspector && bar && inspector->geometry().top() >= bar->geometry().bottom() - 2 * FloatingPanel::shadowMargin(),
          "the inspector starts below the top bar");
    FloatingPanel *dock = view->findChild<FloatingPanel *>("blueprintDock");
    check(dock && inspector && dock->geometry().adjusted(16, 16, -16, -16).right()
                                   < inspector->geometry().adjusted(16, 16, -16, -16).left(),
          "the bottom dock stays clear of the inspector");
    if (!captures.isEmpty()) {
        QDir().mkpath(captures);
        pump(200);
        panel.grab().save(QDir(captures).filePath("blueprint-editor-shell-dark.png"));
    }

    const auto more = menuTexts(bar ? visibleNamed<QPushButton>(*bar, "More blueprint actions") : nullptr);
    check(more.contains("Edit details…") && more.contains("Duplicate") && more.contains("Delete blueprint…")
              && !more.contains("Back to Conversation"),
          "the ⋯ menu holds blueprint actions");

    // The segmented control switches views; the bar sits above the other views' pages.
    if (auto *tests = visibleButton(panel, "Test cases")) tests->click();
    pump(50);
    check(!view->isVisible() && bar && bar->isVisible() && !view->isAncestorOf(bar) && visibleButton(panel, "Start run"),
          "Test cases shows its page under the top bar");
    if (auto *history = visibleButton(panel, "Run history")) history->click();
    pump(50);
    check(bar && bar->isVisible() && visibleText(panel, "Run history") && !view->isVisible(), "Run history is reached from the top bar");
    if (auto *build = visibleButton(panel, "Build")) build->click();
    pump(50);
    check(view->isVisible() && bar && view->isAncestorOf(bar), "Build returns the bar to the canvas");

    // The library dock is the Left overlay, driven by the dock toggle.
    auto *library = visibleButton(*view, "Show node library");
    auto *tree = panel.findChild<BlueprintLibraryDock *>();
    if (library && !library->isChecked()) library->click();
    pump(50);
    check(library && library->isChecked() && tree && tree->isVisible() && view->isAncestorOf(tree),
          "the library toggle shows the node library over the canvas");
    if (library) library->click();
    pump(50);
    check(tree && !tree->isVisible(), "the library toggle hides it again");
    if (library) library->click();
    pump(50);

    panel.resize(782, 628);
    pump(50);
    select(example["nodes"].toArray().at(1).toObject()["id"].toString());
    pump(50);
    check(view->freeArea().width() >= 240, "the compact window keeps a usable canvas beside the inspector");
    check(library && !library->isChecked() && tree && !tree->isVisible(),
          "a crowded compact canvas closes the library and unchecks its toggle");
    panel.resize(1120, 720);
    pump(50);
    check(library && !library->isChecked() && tree && !tree->isVisible(), "the library does not reopen by itself");
    panel.resize(782, 628);
    pump(50);
    const auto clipped = bar ? TextFit::clippedText(bar) : QStringList{"no top bar"};
    check(clipped.isEmpty(), qPrintable("the compact top bar shows its text in full" +
                                        (clipped.isEmpty() ? QString() : " — " + clipped.join("; "))));
    if (!captures.isEmpty()) {
        pump(200);
        panel.grab().save(QDir(captures).filePath("blueprint-editor-shell-compact-dark.png"));
    }
    // Live drafts add Publish, Reload and Open in Conversation; at the narrowest live check width the bar
    // folds the least important of them into the ⋯ menu instead of cutting text.
    if (auto *topBar = qobject_cast<BlueprintTopBar *>(bar)) {
        topBar->setActions(true, true, true, false);
        panel.resize(740, 620);
        pump(50);
        const auto crowded = TextFit::clippedText(bar);
        const auto folded = menuTexts(visibleNamed<QPushButton>(*bar, "More blueprint actions"));
        check(crowded.isEmpty() && visibleButton(*bar, "Publish")
                  && (visibleNamed<QPushButton>(*bar, "Open in Conversation") || folded.contains("Open in Conversation"))
                  && (visibleNamed<QPushButton>(*bar, "Discard draft edits") || folded.contains("Discard draft edits…")),
              qPrintable("a crowded bar folds actions into the ⋯ menu instead of cutting text" +
                         (crowded.isEmpty() ? QString() : " — " + crowded.join("; "))));
        topBar->setActions(false, false, false, false);
    } else check(false, "the top bar is a BlueprintTopBar");
    panel.resize(1120, 720);
    pump(50);

    if (auto *full = bar ? visibleNamed<QPushButton>(*bar, "Fullscreen canvas") : nullptr) full->click();
    pump(50);
    const bool fullscreen = view->window() != &panel && view->window()->isFullScreen() && bar
                            && view->window()->isAncestorOf(bar) && inspector && view->window()->isAncestorOf(inspector);
    if (auto *exit = bar ? visibleNamed<QPushButton>(*bar, "Exit fullscreen") : nullptr) exit->click();
    pump(50);
    check(fullscreen && view->window() == &panel && view->isVisible() && bar && view->isAncestorOf(bar),
          "the view, its panels and the top bar move into fullscreen and back");
    preview.deleteBlueprint(example["id"].toString());
}
}  // namespace

void checkFieldWidgets(BlueprintPreview &preview) {
    const auto input = preview.makeNode("input", "in", 0, 0);
    BlueprintNodeFields fields(preview, "input", input);
    fields.show();
    pump();
    int edits = 0;
    QObject::connect(&fields, &BlueprintNodeFields::edited, [&] { ++edits; });
    auto *name = visibleNamed<QLineEdit>(fields, "Output port name");
    if (name) {
        name->setText("Payload");
        name->setModified(true);
        emit name->editingFinished();
    }
    check(name && fields.node()["out"].toArray().first().toObject()["name"] == "Payload" && fields.error().isEmpty()
              && edits == 1,
          "node fields edit a port name outside the dialog and report the commit");
    check(!fields.portsChanged(), "renaming a port is not a port change");
    auto *type = visibleNamed<QComboBox>(fields, "Output port type");
    if (type) type->setCurrentIndex(type->findData("object"));
    check(type && fields.portsChanged() && edits == 2, "changing a port type is reported as a port change");
    if (name) {
        name->setText("");
        name->setPlaceholderText("");
    }
    check(fields.node().isEmpty() == !fields.error().isEmpty(), "node() is empty exactly when error() explains why");

    QJsonObject model;
    for (const auto &value : preview.example()["nodes"].toArray())
        if (!value.toObject()["model"].toString().isEmpty()) { model = value.toObject(); break; }
    BlueprintBindingFields binding(preview, preview.example(), model);
    binding.show();
    pump();
    QComboBox *models = nullptr;
    for (auto *combo : binding.findChildren<QComboBox *>())
        if (combo->findData(model["model"].toString()) >= 0) { models = combo; break; }
    const int other = models ? (models->currentIndex() + 1) % qMax(1, models->count()) : -1;
    if (models && models->count() > 1) models->setCurrentIndex(other);
    check(models && models->count() > 1 && binding.node()["model"] != model["model"]
              && binding.node()["model"] == models->itemData(other).toString(),
          "binding fields change the checkpoint");

    auto dataset = preview.makeNode("dataset-rows", "rows", 0, 0);
    BlueprintDatasetFields rows(preview, dataset);
    rows.show();
    pump();
    auto *text = rows.findChild<QPlainTextEdit *>();
    if (text) text->setPlainText("[{\"a\": 1}]");
    check(text && rows.node()["setting"] == "[{\"a\": 1}]" && rows.error().isEmpty(),
          "dataset fields edit inline rows");
    if (text) text->setPlainText("not json");
    check(!rows.error().isEmpty(), "dataset fields explain invalid rows");
}

void checkInspector(BlueprintPreview &preview) {
    Theme::setDark(true);
    BlueprintPanel panel(&preview);
    panel.resize(1120, 720);
    panel.show();
    pump();
    const auto in = preview.makeNode("input", "in", 0, 0), out = preview.makeNode("output", "out", 320, 0),
               p = preview.makeNode("parse-json", "p", 0, 240), f = preview.makeNode("field", "f", 320, 240);
    const QJsonObject graph{{"id", "inspect-check"}, {"name", "Inspector check"}, {"revision", 1},
        {"nodes", QJsonArray{in, out, p, f}},
        {"edges", QJsonArray{edge("in", "text", "out", "value"), edge("p", "value", "f", "value")}}};
    preview.saveBlueprint(graph);
    if (auto *library = visibleNamed<QPushButton>(panel, "Back to library")) library->click();
    pump();
    for (auto *button : panel.findChildren<QPushButton *>())
        if (button->isVisible() && button->text() == "Open" && visibleText(*button->parentWidget(), "Inspector check")) {
            button->click();
            break;
        }
    pump(100);
    auto *view = panel.findChild<BlueprintView *>();
    auto *inspector = panel.findChild<BlueprintInspector *>();
    const bool opened = view && inspector && view->graphScene()->node("f");
    check(opened, "the inspector check graph opens");
    if (!opened) return;
    view->fit();
    pump();
    const auto stored = [&] { return preview.blueprint("inspect-check"); };
    const auto storedNode = [&](const QString &id) {
        for (const auto &value : stored()["nodes"].toArray())
            if (value.toObject()["id"] == id) return value.toObject();
        return QJsonObject{};
    };
    const auto select = [&](const QString &id) {
        click(view->viewport(), viewPoint(*view, view->graphScene()->node(id)->sceneBoundingRect().center()));
        pump(50);
    };
    const auto answer = [&](bool accept) {
        QTimer::singleShot(0, &panel, [accept] {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                if (auto *save = accept ? visibleButton(*dialog, "Save changes") : nullptr) save->click();
                else dialog->reject();
            }
        });
    };
    const auto field = [&](const char *name) { return visibleNamed<QLineEdit>(*inspector, name); };

    select("in");
    check(inspector->nodeId() == "in" && inspector->isVisible(), "selecting a node shows it in the inspector");
    if (auto *name = field("Output port name")) {
        name->setText("Payload");
        name->setModified(true);
        emit name->editingFinished();
        pump(50);
    }
    check(storedNode("in")["out"].toArray().first().toObject()["name"] == "Payload",
          "editing a field in the inspector saves through changeNode");
    const auto buttons = panel.findChildren<QPushButton *>();
    check(std::none_of(buttons.cbegin(), buttons.cend(), [](auto *b) { return b->isVisible() && b->text() == "Configure…"; }),
          "the inspector edits in place with no Configure button");

    select("p");
    const auto changeType = [&](bool accept) {
        auto *type = visibleNamed<QComboBox>(*inspector, "Output port type");
        if (!type) return false;
        answer(accept);
        type->setCurrentIndex(type->findData("text"));
        pump(50);
        return true;
    };
    const auto before = stored();
    check(changeType(false) && stored() == before, "a port change that breaks wires asks first and Cancel keeps everything");
    auto *reverted = visibleNamed<QComboBox>(*inspector, "Output port type");
    check(reverted && reverted->currentData() == "object", "declining the change puts the inspector back");
    const auto hasEdge = [&](const QJsonObject &e) { return stored()["edges"].toArray().contains(e); };
    check(changeType(true) && !hasEdge(edge("p", "value", "f", "value"))
              && storedNode("p")["out"].toArray().first().toObject()["type"] == "text",
          "accepting drops the broken wire");

    select("out");
    QPushButton *disconnect = nullptr;
    for (auto *button : inspector->findChildren<QPushButton *>())
        if (button->isVisible() && button->text() == "Disconnect") disconnect = button;
    if (disconnect) disconnect->click();
    pump(50);
    check(disconnect && !hasEdge(edge("in", "text", "out", "value")), "Disconnect removes a wire from the inspector");

    // Typing that was never committed belongs to the node it was typed for.
    select("in");
    const auto outBefore = storedNode("out");
    if (auto *name = field("Output port name")) {
        name->setText("Draft");
        name->setModified(true);
    }
    select("out");
    pump(50);
    check(storedNode("in")["out"].toArray().first().toObject()["name"] == "Draft" && storedNode("out") == outBefore
              && inspector->nodeId() == "out",
          "an edit commits to the node it was typed for when selection moves");
    select("in");
    if (auto *name = field("Output port name")) {
        name->setText("Gone");
        name->setModified(true);
    }
    confirmDelete(&panel);
    key(view->viewport(), Qt::Key_Delete);
    pump(80);
    check(storedNode("in").isEmpty() && storedNode("out") == outBefore, "an edit on a deleted node is dropped");
    preview.deleteBlueprint("inspect-check");
}

void checkLibrary(BlueprintPreview &preview) {
    Theme::setDark(true);
    using Entry = BlueprintLibraryDock::Entry;
    const auto all = BlueprintLibraryDock::groups(preview, {}, {});
    bool legacy = false;
    QHash<QString, QStringList> ops;
    for (const auto &[group, entries] : all)
        for (const auto &entry : entries) {
            legacy |= QStringList{"selector", "extractor", "lookup", "pricing", "tool"}.contains(entry.operation);
            ops[group] << entry.operation;
        }
    check(!all.isEmpty() && !legacy, "legacy operations never appear in the library");
    check(ops["sources"].contains("input") && ops["sources"].contains("dataset-rows") && ops["transform"].contains("count"),
          "operations are grouped by family");
    const auto json = BlueprintLibraryDock::groups(preview, "parse text", {});
    check(json.size() == 1 && json.first().second.size() == 1 && json.first().second.first().operation == "parse-json",
          "search filters operations by label and detail");
    const auto transforms = BlueprintLibraryDock::groups(preview, {}, "transform");
    check(transforms.size() == 1 && transforms.first().first == "transform", "a family chip shows only that family");
    const bool hadSaved = !preview.nodeLibrary().isEmpty();
    preview.saveNodeTemplate("Object input", preview.makeNode("input", "saved-in", 0, 0));
    const auto withSaved = BlueprintLibraryDock::groups(preview, {}, {});
    check(!hadSaved && withSaved.last().first == BlueprintLibraryDock::savedGroup && withSaved.last().second.size() == 1
              && withSaved.last().second.first().savedName == "Object input",
          "saved nodes form a Saved group");
    const Entry roundTrip = Entry::fromJson(withSaved.last().second.first().toJson());
    check(roundTrip.savedId == withSaved.last().second.first().savedId && !roundTrip.savedNode.isEmpty(),
          "a library entry survives the drag payload");

    BlueprintPanel panel(&preview);
    panel.resize(1120, 720);
    panel.show();
    pump();
    preview.saveBlueprint(QJsonObject{{"id", "library-check"}, {"name", "Library check"}, {"revision", 1},
                                      {"nodes", QJsonArray{preview.makeNode("output", "out", 0, 0)}}, {"edges", QJsonArray{}}});
    if (auto *library = visibleNamed<QPushButton>(panel, "Back to library")) library->click();
    pump();
    for (auto *button : panel.findChildren<QPushButton *>())
        if (button->isVisible() && button->text() == "Open" && visibleText(*button->parentWidget(), "Library check")) {
            button->click();
            break;
        }
    pump(100);
    auto *view = panel.findChild<BlueprintView *>();
    auto *dock = panel.findChild<BlueprintLibraryDock *>();
    const bool opened = view && dock && dock->isVisible() && view->graphScene()->node("out");
    check(opened, "the library check graph opens with the dock shown");
    if (!opened) return;
    const auto stored = [&] { return preview.blueprint("library-check")["nodes"].toArray(); };
    const auto added = [&](const QString &op) {
        for (const auto &value : stored())
            if (value.toObject()["operation"] == op) return value.toObject();
        return QJsonObject{};
    };
    QWidget *parseRow = nullptr;
    for (auto *frame : dock->findChildren<QFrame *>("blueprintLibraryRow"))
        if (frame->isVisible() && frame->accessibleName() == "Add Parse JSON") parseRow = frame;
    if (parseRow) click(parseRow, QRectF(parseRow->rect()).center());
    pump(50);
    const auto parse = added("parse-json");
    const auto centre = parse.isEmpty() ? QPointF() : BlueprintScene::nodeRect(parse).center();
    check(parseRow && !parse.isEmpty() && stored().size() == 2 && view->selection() == parse["id"].toString()
              && view->freeArea().contains(view->mapFromScene(centre)),
          "clicking a library row adds the node at once, selected, in the free canvas");

    QMimeData mime;
    mime.setData(BlueprintStyle::entryMimeType, Entry{"count", {}, {}, {}}.toJson());
    // A free corner of the canvas, clear of the nodes already there.
    const QPointF at = QPointF(view->freeArea().topLeft()) + QPointF(24, 24);
    const auto target = view->mapToScene(at.toPoint());
    QDragEnterEvent enter(at, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(view->viewport(), &enter);
    QDropEvent drop(at, Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
    QCoreApplication::sendEvent(view->viewport(), &drop);
    pump(50);
    const auto count = added("count");
    check(!count.isEmpty() && QLineF(QPointF(count["x"].toDouble(), count["y"].toDouble()), target).length() <= 40,
          "dragging a row drops the node there");

    QListWidget *list = nullptr;
    for (auto *candidate : dock->findChildren<QListWidget *>())
        if (candidate->accessibleName() == "Workflow nodes") list = candidate;
    if (auto *tab = visibleButton(*dock, "In this workflow")) tab->click();
    pump();
    bool needsInput = false;
    for (int i = 0; list && i < list->count(); ++i)
        if (list->item(i)->data(Qt::UserRole) == "out") needsInput = list->item(i)->text().contains("needs input");
    check(list && list->isVisible() && list->count() == 3 && needsInput,
          "In this workflow lists the graph's nodes and what needs input");
    for (int i = 0; list && i < list->count(); ++i)
        if (list->item(i)->data(Qt::UserRole) == "out") list->setCurrentRow(i);
    pump(50);
    check(view->selection() == "out", "clicking a workflow row selects that node");

    view->setFocus();
    QShortcutEvent shortcut(QKeySequence(Qt::Key_Slash), 0);
    for (auto *candidate : panel.findChildren<QShortcut *>())
        if (candidate->key() == QKeySequence(Qt::Key_Slash)) QCoreApplication::sendEvent(candidate, &shortcut);
    pump();
    auto *search = visibleNamed<QLineEdit>(*dock, "Search nodes");
    check(search && search->isVisible(), "/ opens the library search");
    for (const auto &value : preview.nodeLibrary()) preview.removeNodeTemplate(value.toObject()["id"].toString());
    preview.deleteBlueprint("library-check");
}

void checkQuickAdd(BlueprintPreview &preview) {
    Theme::setDark(true);
    BlueprintPanel panel(&preview);
    panel.resize(1120, 720);
    panel.show();
    pump();
    preview.saveBlueprint(QJsonObject{{"id", "quick-check"}, {"name", "Quick add check"}, {"revision", 1},
                                      {"nodes", QJsonArray{preview.makeNode("count", "cnt", 0, 0)}}, {"edges", QJsonArray{}}});
    if (auto *library = visibleNamed<QPushButton>(panel, "Back to library")) library->click();
    pump();
    for (auto *button : panel.findChildren<QPushButton *>())
        if (button->isVisible() && button->text() == "Open" && visibleText(*button->parentWidget(), "Quick add check")) {
            button->click();
            break;
        }
    pump(100);
    auto *view = panel.findChild<BlueprintView *>();
    const bool opened = view && view->graphScene()->node("cnt");
    check(opened, "the quick-add check graph opens");
    if (!opened) return;
    const auto stored = [&] { return preview.blueprint("quick-check"); };
    const auto nodesOf = [&](const QString &op) {
        QJsonArray found;
        for (const auto &value : stored()["nodes"].toArray())
            if (value.toObject()["operation"] == op) found.append(value);
        return found;
    };
    const auto menu = [&] { return panel.findChild<BlueprintQuickAdd *>(); };
    const auto search = [&] { return menu() ? menu()->findChild<QLineEdit *>() : nullptr; };
    const auto press = [&](int keyCode, Qt::KeyboardModifiers modifiers = {}) {
        if (auto *field = search()) {
            QKeyEvent event(QEvent::KeyPress, keyCode, modifiers);
            QCoreApplication::sendEvent(field, &event);
        }
        pump(50);
    };
    const auto rows = [&] {
        QStringList texts;
        if (auto *list = menu() ? menu()->findChild<QListWidget *>() : nullptr)
            for (int i = 0; i < list->count(); ++i)
                if (list->item(i)->flags() & Qt::ItemIsSelectable) texts << list->item(i)->text();
        return texts;
    };

    // An empty spot below the node, away from the floating panels.
    const auto node = view->graphScene()->node("cnt")->sceneBoundingRect();
    const QPoint empty = view->mapFromScene(node.bottomLeft() + QPointF(0, 120));
    QCursor::setPos(view->viewport()->mapToGlobal(empty));
    pump();
    key(view->viewport(), Qt::Key_Space, {}, " ");
    pump(50);
    const bool shown = menu() && menu()->isVisible();
    check(shown && search() && panel.window()->focusWidget() == search() && menu()->geometry().adjusted(-24, -24, 24, 24).contains(empty),
          "Space opens quick-add at the cursor with the search focused");
    if (search()) search()->setText("count");
    press(Qt::Key_Return);
    const auto counts = nodesOf("count");
    check(counts.size() == 2 && view->selection() == counts.last().toObject()["id"].toString() && !menu()->isVisible(),
          "Enter adds the highlighted node and selects it");

    key(view->viewport(), Qt::Key_Space, {}, " ");
    pump(50);
    const int before = stored()["nodes"].toArray().size();
    press(Qt::Key_Escape);
    check(menu() && !menu()->isVisible() && stored()["nodes"].toArray().size() == before,
          "Esc closes quick-add without adding");

    // A wire from the Count output (an integer) only offers nodes that can take it.
    view->fit();
    pump();
    auto *cnt = view->graphScene()->node("cnt");
    const auto from = viewPoint(*view, cnt->pin(true, "value")->anchor());
    const auto spot = viewPoint(*view, cnt->sceneBoundingRect().topRight() + QPointF(160, -140));
    drag(view->viewport(), from, spot);
    pump(50);
    const auto offered = rows();
    check(menu() && menu()->isVisible() && !offered.isEmpty() && !offered.contains(preview.operation("count")["label"].toString())
              && offered.contains(preview.operation("output")["label"].toString()),
          "a dropped wire filters quick-add to compatible nodes");
    if (search()) search()->setText(preview.operation("output")["label"].toString());
    press(Qt::Key_Return);
    const auto outputs = nodesOf("output");
    const auto newOutput = outputs.isEmpty() ? QString() : outputs.first().toObject()["id"].toString();
    check(!newOutput.isEmpty() && stored()["edges"].toArray().contains(edge("cnt", "value", newOutput, "value")),
          "the node added from a dropped wire arrives connected");

    if (auto *add = visibleButton(*view, "Add node")) add->click();
    pump(50);
    bool picker = false;
    for (auto *dialog : QApplication::topLevelWidgets())
        picker |= dialog->isVisible() && dialog->accessibleName() == "Add node picker";
    check(!picker && menu() && menu()->isVisible(), "Add node opens quick-add; the modal node picker is gone");
    press(Qt::Key_Escape);
    preview.deleteBlueprint("quick-check");
}

void checkCanvasRun(BlueprintPreview &preview) {
    Theme::setDark(true);
    BlueprintPanel panel(&preview);
    panel.resize(1120, 720);
    panel.show();
    pump();
    const auto example = preview.example();
    const auto graphId = example["id"].toString();
    preview.saveBlueprint(example);
    preview.saveBlueprint(QJsonObject{{"id", "other-run-check"}, {"name", "Other blueprint"}, {"revision", 1},
                                      {"nodes", QJsonArray{preview.makeNode("input", "in", 0, 0)}}, {"edges", QJsonArray{}}});
    const auto open = [&](const QString &name) {
        if (auto *library = visibleNamed<QPushButton>(panel, "Back to library")) library->click();
        pump();
        for (auto *button : panel.findChildren<QPushButton *>())
            if (button->isVisible() && button->text() == "Open" && visibleText(*button->parentWidget(), name)) {
                button->click();
                break;
            }
        pump(100);
    };
    open(example["name"].toString());
    auto *view = panel.findChild<BlueprintView *>();
    auto *bar = panel.findChild<BlueprintTopBar *>();
    const bool opened = view && bar && view->isVisible();
    check(opened, "the run check opens the example on the canvas");
    if (!opened) return;
    const auto statuses = [&] {
        QHash<QString, QString> result;
        for (auto *node : view->graphScene()->nodes()) result.insert(node->id(), node->runStatus());
        return result;
    };
    const auto anyStatus = [&](const QString &status) {
        const auto all = statuses();
        return std::any_of(all.cbegin(), all.cend(), [&](const QString &value) { return value == status; });
    };
    const auto latest = [&] { return preview.runs(graphId).first().toObject()["id"].toString(); };

    if (auto *run = visibleButton(*bar, "Run")) run->click();
    check(waitFor([&] { return anyStatus("running"); }) && bar->view() == BlueprintTopBar::View::Build,
          "Run starts from the Build view and shows the running node");
    const auto runId = preview.runs(graphId).isEmpty() ? QString() : latest();
    bool allPassed = waitForState(preview, runId, "passed");
    pump(50);
    for (const auto &step : preview.run(runId)["steps"].toArray())
        allPassed &= statuses().value(step.toObject()["id"].toString()) == "passed";
    check(!runId.isEmpty() && allPassed, "nodes step through to done");
    FloatingPanel *toast = nullptr;
    for (auto *panelWidget : view->findChildren<FloatingPanel *>())
        if (panelWidget->isVisible() && panelWidget->accessibleName() == "Run result") toast = panelWidget;
    auto *history = toast ? visibleButton(*toast, "Open in Run history") : nullptr;
    check(toast && history && visibleText(*toast, "Completed"), "a toast reports the result");
    if (history) history->click();
    pump(50);
    check(bar->view() == BlueprintTopBar::View::RunHistory && !toast->isVisible(), "the toast opens the run in Run history");
    if (auto *build = visibleButton(panel, "Build")) build->click();
    pump(50);

    bool chosen = false;
    QTimer::singleShot(0, &panel, [&] {
        auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget());
        if (!menu) return;
        for (auto *action : menu->actions())
            if (action->data() == "failure") {
                chosen = true;
                menu->setActiveAction(action);
                QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
                QCoreApplication::sendEvent(menu, &enter);
                return;
            }
        menu->close();
    });
    if (auto *options = visibleNamed<QPushButton>(*bar, "Run options")) options->click();
    pump(50);
    check(chosen, "Run ▾ lists the test cases to run with");
    if (auto *run = visibleButton(*bar, "Run")) run->click();
    pump(50);
    const auto failureId = latest();
    QString failedNode;
    const bool failed = failureId != runId && waitForState(preview, failureId, "failed");
    pump(50);
    for (const auto &step : preview.run(failureId)["steps"].toArray())
        if (step.toObject()["status"] == "failed") failedNode = step.toObject()["id"].toString();
    check(failed && !failedNode.isEmpty() && statuses().value(failedNode) == "failed", "a failed step is marked on the canvas");

    open("Other blueprint");
    check(view->graphScene()->node("in") && !anyStatus("passed") && !anyStatus("failed") && !anyStatus("running"),
          "run marks clear when another blueprint opens");
    preview.deleteBlueprint("other-run-check");
}

void checkDetails(BlueprintPreview &preview) {
    Theme::setDark(true);
    const auto docs = BlueprintDetails::docs();
    QStringList missing;
    for (const auto &value : preview.fixtures()["operations"].toArray()) {
        const auto spec = value.toObject();
        const auto id = spec["id"].toString();
        if (spec["legacyOnly"].toBool() || QStringList{"selector", "extractor", "lookup", "pricing"}.contains(id)) continue;
        if (docs[id].toObject()["about"].toString().isEmpty()) missing << id;
    }
    // The live catalog on dev adds these; the fixture preview cannot build it without a backend.
    for (const auto &id : {"generate", "reason", "lora", "constant", "build-object", "template", "compare", "choose",
                           "validate", "filter", "sort", "search"})
        if (docs[id].toObject()["about"].toString().isEmpty()) missing << id;
    check(missing.isEmpty(), "every library operation has an explanation");
    if (!missing.isEmpty()) fprintf(stderr, "[blueprint-editor] missing docs: %s\n", qPrintable(missing.join(", ")));

    BlueprintPanel panel(&preview);
    panel.resize(1120, 720);
    panel.show();
    pump();
    // A fresh copy of the example, so no earlier check's runs count as its last run.
    auto example = preview.example();
    example["id"] = "details-check";
    example["name"] = "Details check";
    const auto graphId = example["id"].toString();
    preview.saveBlueprint(example);
    if (auto *library = visibleNamed<QPushButton>(panel, "Back to library")) library->click();
    pump();
    for (auto *button : panel.findChildren<QPushButton *>())
        if (button->isVisible() && button->text() == "Open" && visibleText(*button->parentWidget(), example["name"].toString())) {
            button->click();
            break;
        }
    pump(100);
    auto *view = panel.findChild<BlueprintView *>();
    auto *details = panel.findChild<BlueprintDetails *>();
    const auto nodes = example["nodes"].toArray();
    const bool opened = view && details && nodes.size() > 1 && view->graphScene()->node(nodes.at(1).toObject()["id"].toString());
    check(opened, "the details check opens the example");
    if (!opened) return;
    const auto first = nodes.at(0).toObject()["id"].toString();
    const auto second = nodes.at(1).toObject()["id"].toString();
    const auto select = [&](const QString &id) {
        view->fit();
        pump();
        click(view->viewport(), viewPoint(*view, view->graphScene()->node(id)->sceneBoundingRect().center()));
        pump(50);
    };
    const auto tab = [&](const char *name) {
        if (auto *button = visibleNamed<QPushButton>(*details, name)) button->click();
        pump();
    };
    select(first);
    key(view->viewport(), Qt::Key_I, {}, "i");
    pump(50);
    const auto about = docs[BlueprintPreview::operationId(nodes.at(0).toObject())].toObject()["about"].toString();
    check(details->isVisible() && !about.isEmpty() && visibleText(*details, about.left(30)),
          "i opens the details window for the selected node");
    FloatingPanel *inspector = nullptr, *library = nullptr;
    for (auto *panelWidget : view->findChildren<FloatingPanel *>()) {
        if (!panelWidget->isVisible()) continue;
        if (panelWidget->accessibleName() == "Node inspector") inspector = panelWidget;
        if (panelWidget->accessibleName() == "Node library") library = panelWidget;
    }
    const auto box = details->geometry().adjusted(FloatingPanel::shadowMargin(), FloatingPanel::shadowMargin(),
                                                  -FloatingPanel::shadowMargin(), -FloatingPanel::shadowMargin());
    const auto bodyOf = [](FloatingPanel *panelWidget) {
        return panelWidget->geometry().adjusted(FloatingPanel::shadowMargin(), FloatingPanel::shadowMargin(),
                                                -FloatingPanel::shadowMargin(), -FloatingPanel::shadowMargin());
    };
    check((!inspector || !box.intersects(bodyOf(inspector))) && (!library || !box.intersects(bodyOf(library)))
              && view->freeArea().adjusted(-FloatingPanel::shadowMargin(), -FloatingPanel::shadowMargin(),
                                           FloatingPanel::shadowMargin(), FloatingPanel::shadowMargin()).contains(details->geometry()),
          "the details window opens in the free canvas area");
    tab("Last run");
    check(visibleText(*details, "Not run yet"), "Last run says when the node has not run");

    select(second);
    if (auto *info = visibleNamed<QPushButton>(panel, "Node details")) info->click();
    pump(50);
    tab("Inputs & outputs");
    check(details->isVisible() && visibleText(*details, "Wired from " + nodes.at(0).toObject()["name"].toString()),
          "details show what each pin is wired to");

    if (auto *bar = panel.findChild<BlueprintTopBar *>())
        if (auto *run = visibleButton(*bar, "Run")) run->click();
    pump(50);
    const auto runs = preview.runs(graphId);
    check(!runs.isEmpty() && waitForState(preview, runs.first().toObject()["id"].toString(), "passed"), "the example runs");
    select(first);
    key(view->viewport(), Qt::Key_I, {}, "i");
    pump(50);
    tab("Last run");
    check(details->isVisible() && visibleText(*details, "Completed"), "Last run shows the latest step");

    details->showFor("unheard-op", {}, {}, {}, QRect(40, 40, 10, 10));
    pump();
    check(details->isVisible() && visibleText(*details, "No description"), "an operation without docs still shows details");
    key(details, Qt::Key_Escape);
    QShortcutEvent escape(QKeySequence(Qt::Key_Escape), 0);
    for (auto *shortcut : details->findChildren<QShortcut *>()) QCoreApplication::sendEvent(shortcut, &escape);
    pump();
    check(!details->isVisible(), "Esc closes the details window");
    preview.deleteBlueprint("details-check");
}

void checkPolish(BlueprintPreview &preview, const QString &captures) {
    auto example = preview.example();
    example["id"] = "polish-check";
    example["name"] = "Polish check";
    preview.saveBlueprint(example);
    const auto firstId = example["nodes"].toArray().at(0).toObject()["id"].toString();
    BlueprintPanel panel(&preview);
    panel.resize(1104, 720);
    panel.show();
    pump();
    const auto openGraph = [&] {
        if (auto *library = visibleNamed<QPushButton>(panel, "Back to library")) library->click();
        pump();
        for (auto *button : panel.findChildren<QPushButton *>())
            if (button->isVisible() && button->text() == "Open" && visibleText(*button->parentWidget(), "Polish check")) {
                button->click();
                break;
            }
        pump(100);
    };
    openGraph();
    auto *view = panel.findChild<BlueprintView *>();
    auto *details = panel.findChild<BlueprintDetails *>();
    if (!view || !details || !view->graphScene()->node(firstId)) {
        check(false, "the polish check opens its graph");
        return;
    }
    const auto capture = [&](QWidget &widget, const QString &name) {
        if (captures.isEmpty()) return;
        QDir().mkpath(captures);
        check(widget.grab().save(QDir(captures).filePath(name + ".png")), "saved editor reference capture");
    };
    const auto select = [&](const QString &id) {
        click(view->viewport(), viewPoint(*view, view->graphScene()->node(id)->sceneBoundingRect().center()));
        pump(50);
    };
    const auto openQuickAdd = [&] {
        QCursor::setPos(view->viewport()->mapToGlobal(view->freeArea().center()));
        key(view->viewport(), Qt::Key_Space, {}, " ");
        pump(50);
        return panel.findChild<BlueprintQuickAdd *>();
    };

    const int storedSize = QSettings().value("appearance/interfaceSize", 1).toInt();
    for (const bool dark : {true, false})
        for (const int size : {1, 2})
            for (const int width : {1104, 800}) {
                QSettings().setValue("appearance/interfaceSize", size);
                Theme::applyPreferences();
                Theme::setDark(dark);
                panel.resize(width, 720);
                pump(150);
                view->fit();
                select(firstId);
                QStringList found = TextFit::clippedText(view);
                const auto name = QString("%1/%2/%3").arg(dark ? "dark" : "light", size == 2 ? "Large" : "Default").arg(width);
                if (auto *menu = openQuickAdd()) {
                    found << TextFit::clippedText(menu);
                    if (size == 1 && width == 1104) capture(panel, QString("blueprints-quick-add-%1").arg(dark ? "dark" : "light"));
                    menu->dismiss();
                }
                key(view->viewport(), Qt::Key_I, {}, "i");
                pump(50);
                if (details->isVisible()) {
                    found << TextFit::clippedText(details);
                    if (size == 1 && width == 1104) capture(panel, QString("blueprints-details-%1").arg(dark ? "dark" : "light"));
                    details->hide();
                }
                if (size == 1) capture(panel, QString("blueprints-editor-%1%2").arg(dark ? "dark" : "light", width == 800 ? "-compact" : ""));
                found.removeDuplicates();
                check(found.isEmpty(), qPrintable(TextFit::describe(name, "Blueprints editor", found)));
            }
    QSettings().setValue("appearance/interfaceSize", storedSize);
    Theme::applyPreferences();
    Theme::setDark(true);
    panel.resize(1104, 720);
    pump(150);

    // The editor from the keyboard alone.
    view->fit();
    pump();
    QFocusEvent focusIn(QEvent::FocusIn, Qt::TabFocusReason);
    QCoreApplication::sendEvent(view, &focusIn);
    view->setFocus(Qt::TabFocusReason);
    bool keyboard = true;
    const auto step = [&](bool passed, const char *what) {
        if (!passed) fprintf(stderr, "[blueprint-editor] keyboard step failed: %s\n", what);
        keyboard &= passed;
    };
    const int nodesBefore = preview.blueprint("polish-check")["nodes"].toArray().size();
    auto *menu = openQuickAdd();
    step(menu && menu->isVisible(), "Space opens quick-add");
    if (auto *field = menu ? menu->findChild<QLineEdit *>() : nullptr) {
        field->setText("Count items");
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QCoreApplication::sendEvent(field, &enter);
        pump(50);
    }
    step(preview.blueprint("polish-check")["nodes"].toArray().size() == nodesBefore + 1, "Enter adds the node");
    QShortcutEvent slash(QKeySequence(Qt::Key_Slash), 0);
    for (auto *shortcut : panel.findChildren<QShortcut *>())
        if (shortcut->key() == QKeySequence(Qt::Key_Slash)) QCoreApplication::sendEvent(shortcut, &slash);
    pump();
    auto *search = visibleNamed<QLineEdit>(panel, "Search nodes");
    step(search && panel.window()->focusWidget() == search, "/ focuses the library search");
    view->setFocus(Qt::TabFocusReason);
    const auto before = view->centerInGraph();
    key(view->viewport(), Qt::Key_Right);
    pump();
    step(view->centerInGraph() != before, "arrows pan");
    int zoomed = -1;
    const auto watch = QObject::connect(view, &BlueprintView::zoomChanged, [&](int percent) { zoomed = percent; });
    key(view->viewport(), Qt::Key_F, {}, "f");
    pump();
    QObject::disconnect(watch);
    step(zoomed > 0, "F fits");
    view->setSelection(firstId);
    key(view->viewport(), Qt::Key_I, {}, "i");
    pump(50);
    step(details->isVisible(), "i opens details");
    QShortcutEvent escape(QKeySequence(Qt::Key_Escape), 0);
    for (auto *shortcut : details->findChildren<QShortcut *>()) QCoreApplication::sendEvent(shortcut, &escape);
    pump();
    step(!details->isVisible(), "Esc closes details");
    select(firstId);
    QSet<QWidget *> chain;
    for (QWidget *w = view->nextInFocusChain(); w && w != view && !chain.contains(w); w = w->nextInFocusChain()) chain.insert(w);
    const auto reachable = [&](QWidget *target) {
        return target && chain.contains(target) && (target->focusPolicy() & Qt::TabFocus);
    };
    step(reachable(visibleNamed<QLineEdit>(panel, "Name (optional)")) && reachable(visibleNamed<QPushButton>(panel, "Delete node")),
         "Tab reaches the inspector's fields and Delete node");
    check(keyboard, "every editor action works from the keyboard");

    // Dragging a node to the canvas edge keeps scrolling.
    view->fit();
    pump();
    const auto centre = view->centerInGraph();
    const auto from = viewPoint(*view, view->graphScene()->node(firstId)->sceneBoundingRect().center());
    drag(view->viewport(), from, QPointF(view->viewport()->width() - 4, from.y()), 250);
    pump();
    check(view->centerInGraph().x() > centre.x() + 20, "dragging a node to the edge scrolls the canvas");
    preview.deleteBlueprint("polish-check");
}

int runBlueprintEditorChecks(const QString &captures) {
    g_failures = 0;
    const bool original = Theme::isDark();
    Theme::setDark(true);
    BlueprintPreview preview(nullptr, false);
    checkStyle();
    checkScene(preview);
    checkView(preview);
    checkConnecting(preview);
    checkPanelCanvas(preview);
    checkOverlays(preview);
    checkShell(preview, captures);
    checkFieldWidgets(preview);
    checkInspector(preview);
    checkLibrary(preview);
    checkQuickAdd(preview);
    checkCanvasRun(preview);
    checkDetails(preview);
    checkPolish(preview, captures);
    Theme::setDark(original);
    fprintf(stderr, "[blueprint-editor] %s (%d failures)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures;
}
