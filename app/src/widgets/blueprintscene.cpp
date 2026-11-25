#include "widgets/blueprintscene.h"

#include "panels/blueprintpreview.h"

#include <QJsonArray>
#include <algorithm>

BlueprintScene::BlueprintScene(const BlueprintPreview &preview, QObject *parent)
    : QGraphicsScene(parent), m_preview(preview) {}

QRectF BlueprintScene::nodeRect(const QJsonObject &node) {
    return QRectF(QPointF(node["x"].toDouble(), node["y"].toDouble()), NodeItem::sizeFor(node));
}

QList<NodeItem *> BlueprintScene::nodes() const { return m_order; }
QList<WireItem *> BlueprintScene::wires() const { return m_wires; }

void BlueprintScene::setGraph(const QJsonObject &graph) {
    clear();
    m_nodes.clear();
    m_order.clear();
    m_wires.clear();
    m_graph = graph;
    for (const auto &value : graph["nodes"].toArray()) {
        const auto data = value.toObject();
        auto *item = new NodeItem(data, m_preview.operation(BlueprintPreview::operationId(data)), m_preview);
        addItem(item);
        m_nodes.insert(item->id(), item);
        m_order << item;
    }
    for (const auto &value : graph["edges"].toArray()) {
        const auto edge = value.toObject();
        auto *from = m_nodes.value(edge["from"].toString());
        auto *to = m_nodes.value(edge["to"].toString());
        auto *out = from ? from->pin(true, edge["out"].toString()) : nullptr;
        auto *in = to ? to->pin(false, edge["in"].toString()) : nullptr;
        if (!out || !in) continue;
        auto *wire = new WireItem(edge, out, in);
        addItem(wire);
        m_wires << wire;
    }
    for (auto *item : m_order) {
        connect(item, &NodeItem::moved, this, [this] {
            for (auto *wire : m_wires) wire->updatePath();
        });
    }
    refreshWarnings();
}

QJsonObject BlueprintScene::graph() const { return m_graph; }

void BlueprintScene::refreshWarnings() {
    const auto issues = m_preview.issues(m_graph);
    const auto edges = m_graph["edges"].toArray();
    for (auto *item : m_order) {
        const auto data = item->node();
        QStringList lines;
        for (const auto &value : data["in"].toArray()) {
            const auto port = value.toObject();
            const bool connected = std::any_of(edges.cbegin(), edges.cend(), [&](const auto &edge) {
                return edge.toObject()["to"] == data["id"] && edge.toObject()["in"] == port["id"];
            });
            if (!connected) lines << m_preview.portLabel(port) + " is not connected";
        }
        const auto name = data["name"].toString();
        if (!name.isEmpty())
            for (const auto &issue : issues)
                if (issue.startsWith(name + " ")) lines << issue;
        item->setWarning(lines.join('\n'));
    }
}
