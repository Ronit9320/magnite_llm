#include "widgets/blueprintview.h"

#include "theme.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QCursor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>
#include <QWheelEvent>
#include <QtMath>
#include <cmath>

namespace {
constexpr qreal gridStep = 24;
constexpr qreal dotsMinZoom = 0.35;
constexpr qreal minZoom = 0.25;
constexpr qreal maxZoom = 2.5;
constexpr qreal fitFloor = 0.05;
constexpr qreal keyPan = 45;
constexpr qreal sceneExtent = 100000;
}  // namespace

BlueprintView::BlueprintView(const BlueprintPreview &preview, QWidget *parent)
    : QGraphicsView(parent), m_scene(new BlueprintScene(preview, this)) {
    setObjectName("blueprintView");
    setProperty("transientScroll", true);  // the wheel zooms here; SmoothWheel must not scroll it
    setAccessibleName("Workflow graph");
    setAccessibleDescription("Drag the background to pan and drag a node to move it. Scroll to zoom. "
                             "Use the arrow keys to pan, plus or minus to zoom, and F or Home to fit. "
                             "Press Delete to remove the selected node, Space or Tab to add a node, and I for its details.");
    setScene(m_scene);
    m_scene->setSceneRect(-sceneExtent, -sceneExtent, 2 * sceneExtent, 2 * sceneExtent);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(240, 275);
    setFrameShape(QFrame::NoFrame);
    setDragMode(QGraphicsView::NoDrag);
    setRubberBandSelectionMode(Qt::IntersectsItemShape);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setTransformationAnchor(QGraphicsView::NoAnchor);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setRenderHints(QPainter::Antialiasing | QPainter::TextAntialiasing | QPainter::SmoothPixmapTransform);
    setViewportUpdateMode(QGraphicsView::SmartViewportUpdate);
    setCacheMode(QGraphicsView::CacheBackground);
    connect(m_scene, &QGraphicsScene::selectionChanged, this, &BlueprintView::onSelectionChanged);
}

int BlueprintView::zoomPercent() const { return qRound(zoom() * 100); }

QPointF BlueprintView::centerInGraph() const {
    return mapToScene(QPoint(viewport()->width() / 2, viewport()->height() / 2));
}

void BlueprintView::setGraph(const QJsonObject &graph) {
    const QString previous = m_selected;
    m_quiet = true;
    m_dragged = nullptr;
    m_scene->setGraph(graph);
    m_quiet = false;
    if (auto *item = m_scene->node(previous)) {
        m_quiet = true;
        item->setSelected(true);
        m_quiet = false;
    } else if (!previous.isEmpty()) {
        m_selected.clear();
        emit nodeSelected({});
    }
    applyRunStates();
    if (m_autoFit) fit();
}

QString BlueprintView::selection() const { return m_selected; }

void BlueprintView::setSelection(const QString &nodeId) {
    m_quiet = true;
    m_scene->clearSelection();
    m_selected.clear();
    if (auto *item = m_scene->node(nodeId)) {
        item->setSelected(true);
        m_selected = nodeId;
    }
    m_quiet = false;
}

void BlueprintView::onSelectionChanged() {
    if (m_quiet) return;
    QString id;
    for (auto *node : m_scene->nodes())
        if (node->isSelected()) {
            id = node->id();
            break;
        }
    if (id == m_selected) return;
    m_selected = id;
    emit nodeSelected(id);
}

void BlueprintView::fit() {
    QRectF extent;
    for (auto *node : m_scene->nodes())
        extent = extent.united(QRectF(node->pos(), NodeItem::sizeFor(node->node())).adjusted(-24, -34, 24, 28));
    if (extent.isEmpty()) extent = QRectF(0, 0, 680, 438);
    m_autoFit = true;
    const QSize size = viewport()->size();
    const qreal value = qBound(fitFloor, qMin((size.width() - 26) / extent.width(), (size.height() - 26) / extent.height()), qreal(1));
    setTransform(QTransform::fromScale(value, value));
    centerOn(extent.center());
    emit zoomChanged(zoomPercent());
    viewport()->update();
}

void BlueprintView::zoomAt(qreal factor, const QPointF &viewPosition) {
    const qreal current = zoom();
    const qreal next = qBound(qMin(minZoom, current), current * factor, maxZoom);
    m_autoFit = false;
    if (qFuzzyCompare(next, current)) return;
    const QPointF anchored = mapToScene(viewPosition.toPoint());
    setTransform(QTransform::fromScale(next, next));
    centerOn(centerInGraph() + anchored - mapToScene(viewPosition.toPoint()));
    emit zoomChanged(zoomPercent());
}

void BlueprintView::zoomBy(qreal factor) {
    zoomAt(factor, QPointF(viewport()->width() / 2.0, viewport()->height() / 2.0));
}

void BlueprintView::panBy(const QPointF &viewDelta) {
    m_autoFit = false;
    centerOn(centerInGraph() - viewDelta / zoom());
}

void BlueprintView::revealNode(const QString &id) {
    if (auto *item = m_scene->node(id)) {
        m_autoFit = false;
        centerOn(QRectF(item->pos(), NodeItem::sizeFor(item->node())).center());
    }
}

void BlueprintView::setRunStates(const QHash<QString, QString> &statusByNode, const QString &activeNode) {
    m_status = statusByNode;
    m_activeNode = activeNode;
    applyRunStates();
}

void BlueprintView::clearRunStates() { setRunStates({}, {}); }

void BlueprintView::applyRunStates() {
    for (auto *node : m_scene->nodes()) node->setRunStatus(m_status.value(node->id()));
    for (auto *wire : m_scene->wires())
        wire->setActive(!m_activeNode.isEmpty() && wire->edge()["to"].toString() == m_activeNode);
}

void BlueprintView::drawBackground(QPainter *painter, const QRectF &rect) {
    painter->fillRect(rect, QColor(Theme::canvasBackground()));
    if (zoom() < dotsMinZoom) return;
    QPen pen{QColor(Theme::blueprintCanvasDot())};
    pen.setWidthF(2.5);
    pen.setCosmetic(true);
    pen.setCapStyle(Qt::RoundCap);
    painter->setPen(pen);
    QVector<QPointF> dots;
    const qreal left = std::floor(rect.left() / gridStep) * gridStep;
    const qreal top = std::floor(rect.top() / gridStep) * gridStep;
    for (qreal y = top; y <= rect.bottom(); y += gridStep)
        for (qreal x = left; x <= rect.right(); x += gridStep) dots.append({x, y});
    painter->drawPoints(dots.constData(), dots.size());
}

void BlueprintView::resizeEvent(QResizeEvent *event) {
    QGraphicsView::resizeEvent(event);
    if (m_autoFit) fit();
}

void BlueprintView::changeEvent(QEvent *event) {
    QGraphicsView::changeEvent(event);
    if (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange) {
        resetCachedContent();
        viewport()->update();
    }
}

void BlueprintView::wheelEvent(QWheelEvent *event) {
    const qreal delta = event->pixelDelta().isNull() ? event->angleDelta().y() : event->pixelDelta().y();
    if (delta != 0) {
        zoomAt(std::exp(delta * .0016), event->position());
        event->accept();
    } else event->ignore();
}

NodeItem *BlueprintView::nodeAt(const QPoint &viewPosition) const {
    for (auto *item : items(viewPosition))
        for (auto *node : m_scene->nodes())
            if (node == item || node->isAncestorOf(item)) return node;
    return nullptr;
}

void BlueprintView::mousePressEvent(QMouseEvent *event) {
    const bool left = event->button() == Qt::LeftButton, middle = event->button() == Qt::MiddleButton;
    if (!left && !middle) {
        event->accept();
        return;
    }
    setFocus(Qt::MouseFocusReason);
    auto *node = left ? nodeAt(event->position().toPoint()) : nullptr;
    if (node) {
        m_dragged = node;
        m_dragStart = node->pos();
        QGraphicsView::mousePressEvent(event);
        return;
    }
    m_panning = true;
    m_panMoved = false;
    m_pressPos = event->position();
    m_panStartCenter = centerInGraph();
    event->accept();
}

void BlueprintView::mouseMoveEvent(QMouseEvent *event) {
    if (m_panning) {
        const QPointF delta = event->position() - m_pressPos;
        if (!m_panMoved && delta.manhattanLength() >= QApplication::startDragDistance()) {
            m_panMoved = true;
            m_autoFit = false;
            viewport()->setCursor(Qt::ClosedHandCursor);
        }
        if (m_panMoved) centerOn(m_panStartCenter - delta / zoom());
        event->accept();
        return;
    }
    if (m_dragged && event->buttons().testFlag(Qt::LeftButton)) {
        m_autoFit = false;
        viewport()->setCursor(Qt::ClosedHandCursor);
    }
    QGraphicsView::mouseMoveEvent(event);
}

void BlueprintView::mouseReleaseEvent(QMouseEvent *event) {
    if (m_panning) {
        if (event->button() == Qt::LeftButton || event->button() == Qt::MiddleButton) {
            m_panning = false;
            viewport()->unsetCursor();
            if (!m_panMoved && event->button() == Qt::LeftButton) m_scene->clearSelection();
            event->accept();
        }
        return;
    }
    QGraphicsView::mouseReleaseEvent(event);
    if (m_dragged && event->button() == Qt::LeftButton) {
        NodeItem *node = std::exchange(m_dragged, nullptr);
        viewport()->unsetCursor();
        if (node->pos().toPoint() != m_dragStart.toPoint()) emit nodeMoved(node->id(), node->pos().toPoint());
    }
}

void BlueprintView::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && !nodeAt(event->position().toPoint())) {
        emit quickAddRequested(event->globalPosition().toPoint(), mapToScene(event->position().toPoint()));
        event->accept();
        return;
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

void BlueprintView::contextMenuEvent(QContextMenuEvent *event) {
    const bool keyboard = event->reason() == QContextMenuEvent::Keyboard;
    const QPoint at = keyboard ? QPoint(viewport()->width() / 2, viewport()->height() / 2) : event->pos();
    const QPoint global = keyboard ? viewport()->mapToGlobal(at) : event->globalPos();
    NodeItem *node = keyboard ? m_scene->node(m_selected) : nodeAt(at);
    if (node) {
        if (node->id() != m_selected) {
            m_scene->clearSelection();
            node->setSelected(true);
        }
        emit nodeMenuRequested(node->id(), global, mapToScene(at));
    } else emit canvasMenuRequested(global, mapToScene(at));
    event->accept();
}

void BlueprintView::requestQuickAdd() {
    QPoint at = viewport()->mapFromGlobal(QCursor::pos());
    if (!viewport()->rect().contains(at)) at = QPoint(viewport()->width() / 2, viewport()->height() / 2);
    emit quickAddRequested(viewport()->mapToGlobal(at), mapToScene(at));
}

bool BlueprintView::event(QEvent *event) {
    // Tab adds a node here; Shift+Tab still leaves the canvas.
    if (event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Tab && key->modifiers() == Qt::NoModifier) {
            requestQuickAdd();
            key->accept();
            return true;
        }
    }
    return QGraphicsView::event(event);
}

void BlueprintView::keyPressEvent(QKeyEvent *event) {
    switch (event->key()) {
    case Qt::Key_Plus: case Qt::Key_Equal: zoomBy(1.2); break;
    case Qt::Key_Minus: zoomBy(1 / 1.2); break;
    case Qt::Key_Home: case Qt::Key_F: fit(); break;
    case Qt::Key_Delete: case Qt::Key_Backspace:
        if (m_selected.isEmpty()) { event->ignore(); return; }
        emit deleteRequested(m_selected);
        break;
    case Qt::Key_Space: requestQuickAdd(); break;
    case Qt::Key_I:
        if (m_selected.isEmpty()) { event->ignore(); return; }
        emit detailsRequested(m_selected);
        break;
    case Qt::Key_Left: panBy({keyPan, 0}); break;
    case Qt::Key_Right: panBy({-keyPan, 0}); break;
    case Qt::Key_Up: panBy({0, keyPan}); break;
    case Qt::Key_Down: panBy({0, -keyPan}); break;
    default: QGraphicsView::keyPressEvent(event); return;
    }
    event->accept();
}
