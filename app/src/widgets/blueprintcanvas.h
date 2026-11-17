#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QWidget>

class QTimer;

class BlueprintCanvas : public QWidget {
    Q_OBJECT
public:
    explicit BlueprintCanvas(QWidget *parent = nullptr);

    void setGraph(const QJsonObject &graph, const QJsonObject &fixtures);
    void setSelection(const QString &node, const QString &outputNode, const QString &outputPort);
    void fit();
    void zoomBy(qreal factor);
    void revealNode(const QString &id);
    QPointF centerInGraph() const;
    static QString nodeIcon(const QString &kind);
    static QColor nodeAccent(const QString &kind);

signals:
    void zoomChanged(int percent);
    void nodeSelected(const QString &id);
    void nodeMenuRequested(const QString &id, const QPoint &screenPosition, const QPointF &graphPosition);
    void deleteRequested(const QString &id);
    void nodeMoved(const QString &id, const QPoint &position);
    void portSelected(const QString &node, bool output, const QString &port);

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;

private:
    QRectF nodeRect(const QJsonObject &node) const;
    QPointF portCenter(const QJsonObject &node, bool output, int index) const;
    int portIndex(const QJsonObject &node, bool output, const QString &id) const;
    void drawConnections(QPainter &painter) const;
    void drawNode(QPainter &painter, const QJsonObject &node) const;
    QString nodeAt(const QPointF &point) const;
    void moveNode();
    void panAtEdge();
    void zoomAt(qreal factor, const QPointF &position);
    QPointF graphPoint(const QPointF &point) const { return (point - m_origin) / m_zoom; }

    QJsonObject m_fixtures;
    QJsonArray m_edges;
    QList<QString> m_order;
    QHash<QString, QJsonObject> m_nodes;
    QString m_selected;
    QString m_armedNode;
    QString m_armedPort;
    QString m_dragged;
    QPointF m_origin;
    QPointF m_press;
    QPointF m_dragStart;
    QPointF m_pointer;
    QTimer *m_edgePan;
    qreal m_zoom = 1;
    bool m_autoFit = true;
    bool m_panning = false;
    bool m_dragging = false;
};
