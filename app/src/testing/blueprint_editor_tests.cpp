#include "testing/blueprint_editor_tests.h"

#include "panels/blueprintpreview.h"
#include "testing/blueprint_test_support.h"
#include "theme.h"
#include "widgets/blueprintscene.h"
#include "widgets/blueprintstyle.h"

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
    scene.setGraph(preview.example());
    scene.wires().first()->setActive(true);
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
    auto in = odd["in"].toArray();
    auto p = in[0].toObject();
    p["type"] = "mystery";
    in[0] = p;
    odd["in"] = in;
    scene.setGraph({{"nodes", QJsonArray{odd}}});
    check(scene.node("odd") && scene.node("odd")->pin(false, "value"), "unknown kinds and port types still render");
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
    Theme::setDark(original);
    fprintf(stderr, "[blueprint-editor] %s (%d failures)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures;
}
