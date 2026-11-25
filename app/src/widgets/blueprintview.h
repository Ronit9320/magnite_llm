#pragma once

#include "widgets/blueprintscene.h"

#include <QGraphicsView>
#include <QHash>
#include <QJsonObject>

class BlueprintPreview;

// The Blueprints canvas: camera (pan, zoom, fit), selection, node moving and the outward signals.
class BlueprintView : public QGraphicsView {
    Q_OBJECT
public:
    explicit BlueprintView(const BlueprintPreview &preview, QWidget *parent = nullptr);

    BlueprintScene *graphScene() const { return m_scene; }
    void setGraph(const QJsonObject &graph);
    void setSelection(const QString &nodeId);
    QString selection() const;
    void fit();
    void zoomBy(qreal factor);
    int zoomPercent() const;
    void revealNode(const QString &id);
    QPointF centerInGraph() const;
    void setRunStates(const QHash<QString, QString> &statusByNode, const QString &activeNode);
    void clearRunStates();

signals:
    void zoomChanged(int percent);
    void nodeSelected(const QString &id);
    void nodeMenuRequested(const QString &id, const QPoint &screenPosition, const QPointF &graphPosition);
    void canvasMenuRequested(const QPoint &screenPosition, const QPointF &graphPosition);
    void quickAddRequested(const QPoint &screenPosition, const QPointF &graphPosition);
    void detailsRequested(const QString &id);
    void deleteRequested(const QString &id);
    void nodeMoved(const QString &id, const QPoint &position);

protected:
    void drawBackground(QPainter *painter, const QRectF &rect) override;
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool event(QEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    NodeItem *nodeAt(const QPoint &viewPosition) const;
    void zoomAt(qreal factor, const QPointF &viewPosition);
    void panBy(const QPointF &viewDelta);
    void applyRunStates();
    void onSelectionChanged();
    void requestQuickAdd();
    qreal zoom() const { return transform().m11(); }

    BlueprintScene *m_scene;
    QString m_selected;
    QHash<QString, QString> m_status;
    QString m_activeNode;
    bool m_autoFit = true;
    bool m_quiet = false;
    bool m_panning = false;
    bool m_panMoved = false;
    QPointF m_pressPos;
    QPointF m_panStartCenter;
    NodeItem *m_dragged = nullptr;
    QPointF m_dragStart;
};
