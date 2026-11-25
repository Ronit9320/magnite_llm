#include "widgets/blueprintminimap.h"

#include "theme.h"
#include "widgets/blueprintitems.h"
#include "widgets/blueprintstyle.h"
#include "widgets/blueprintview.h"

#include <QMouseEvent>
#include <QPainter>
#include <QScrollBar>

namespace {
constexpr int inset = 8;
constexpr int radius = 10;
}  // namespace

BlueprintMinimap::BlueprintMinimap(BlueprintView *view) : QWidget(view), m_view(view) {
    setObjectName("blueprintMinimap");
    setAccessibleName("Graph overview");
    setAccessibleDescription("Click or drag to move the view around the graph.");
    setFixedSize(width, height);
    connect(view->graphScene(), &QGraphicsScene::changed, this, qOverload<>(&QWidget::update));
    connect(view, &BlueprintView::zoomChanged, this, qOverload<>(&QWidget::update));
    connect(view->horizontalScrollBar(), &QScrollBar::valueChanged, this, qOverload<>(&QWidget::update));
    connect(view->verticalScrollBar(), &QScrollBar::valueChanged, this, qOverload<>(&QWidget::update));
}

QRectF BlueprintMinimap::sceneExtent() const {
    QRectF extent = m_view->mapToScene(m_view->viewport()->rect()).boundingRect();
    for (auto *node : m_view->graphScene()->nodes())
        extent = extent.united(QRectF(node->pos(), NodeItem::sizeFor(node->node())));
    return extent.adjusted(-24, -24, 24, 24);
}

QTransform BlueprintMinimap::toWidget() const {
    if (m_frozen) return *m_frozen;
    const QRectF extent = sceneExtent();
    const QRectF area = QRectF(rect()).adjusted(inset, inset, -inset, -inset);
    const qreal scale = qMin(area.width() / extent.width(), area.height() / extent.height());
    const QPointF origin = area.center() - extent.center() * scale;
    return QTransform(scale, 0, 0, scale, origin.x(), origin.y());
}

void BlueprintMinimap::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(Theme::blueprintPanelFill()));
    painter.drawRoundedRect(rect(), radius, radius);

    const QTransform map = toWidget();
    for (auto *node : m_view->graphScene()->nodes()) {
        QColor color = BlueprintStyle::familyColor(node->family());
        color.setAlphaF(0.85);
        painter.setBrush(color);
        painter.drawRoundedRect(map.mapRect(QRectF(node->pos(), NodeItem::sizeFor(node->node()))), 1.5, 1.5);
    }

    QColor accent(Theme::accentPrimary());
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(accent, 1.5));
    const QRectF visible = map.mapRect(m_view->mapToScene(m_view->viewport()->rect()).boundingRect());
    painter.setClipRect(rect().adjusted(1, 1, -1, -1));
    painter.drawRect(visible);
}

void BlueprintMinimap::centreAt(const QPointF &widgetPosition) {
    bool invertible = false;
    const QTransform inverse = toWidget().inverted(&invertible);
    if (invertible) m_view->centerAt(inverse.map(widgetPosition));
}

void BlueprintMinimap::mousePressEvent(QMouseEvent *event) {
    event->accept();
    if (event->button() != Qt::LeftButton) return;
    m_frozen = toWidget();
    centreAt(event->position());
}

void BlueprintMinimap::mouseMoveEvent(QMouseEvent *event) {
    event->accept();
    if (event->buttons() & Qt::LeftButton) centreAt(event->position());
}

void BlueprintMinimap::mouseReleaseEvent(QMouseEvent *event) {
    event->accept();
    m_frozen.reset();
    update();
}
