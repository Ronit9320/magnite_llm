#pragma once

#include "widgets/blueprintitems.h"

#include <QGraphicsScene>
#include <QHash>
#include <QJsonObject>

class BlueprintPreview;

// The graph as graphics items: one NodeItem per node, one WireItem per edge.
class BlueprintScene : public QGraphicsScene {
    Q_OBJECT
public:
    explicit BlueprintScene(const BlueprintPreview &preview, QObject *parent = nullptr);

    void setGraph(const QJsonObject &graph);
    QJsonObject graph() const;
    NodeItem *node(const QString &id) const { return m_nodes.value(id); }
    QList<NodeItem *> nodes() const;
    QList<WireItem *> wires() const;
    static QRectF nodeRect(const QJsonObject &node);
    // The warning lines a node shows: unconnected inputs, then the graph's issues that name it.
    static QStringList warningLines(const BlueprintPreview &preview, const QJsonObject &graph,
                                    const QJsonObject &node, const QStringList &issues);

private:
    void refreshWarnings();

    const BlueprintPreview &m_preview;
    QJsonObject m_graph;
    QHash<QString, NodeItem *> m_nodes;
    QList<NodeItem *> m_order;
    QList<WireItem *> m_wires;
};
