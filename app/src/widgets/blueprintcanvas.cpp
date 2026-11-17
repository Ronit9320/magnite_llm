#include "widgets/blueprintcanvas.h"

#include "theme.h"

#include <QApplication>
#include <QContextMenuEvent>
#include <QJsonArray>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QTimer>
#include <QWheelEvent>
#include <algorithm>
#include <cmath>
#include <utility>

namespace {
constexpr int nodeWidth = 190;
constexpr int portSpacing = 22;
constexpr int firstPort = 102;
constexpr qreal minZoom = .01;
QColor portColor(const QString &type) {
    if (type == "index" || type == "integer" || type == "number") return QColor(Theme::accentLabel());
    if (type == "fields" || type == "order" || type == "receipt" || type == "object" || type == "array") return QColor(Theme::statusSuccess());
    if (type == "boolean") return QColor(Theme::statusWarning());
    return QColor(Theme::comparisonSeries());
}
QPainterPath roundedCable(std::initializer_list<QPointF> points) {
    const QList<QPointF> route(points);
    QPainterPath path(route.first());
    for (int i = 1; i + 1 < route.size(); ++i) {
        const auto corner = route[i];
        const auto incoming = route[i - 1] - corner;
        const auto outgoing = route[i + 1] - corner;
        const qreal before = std::hypot(incoming.x(), incoming.y());
        const qreal after = std::hypot(outgoing.x(), outgoing.y());
        if (before < .01 || after < .01) {
            path.lineTo(corner);
            continue;
        }
        const qreal radius = qMin(16.0, qMin(before, after) * .45);
        path.lineTo(corner + incoming * (radius / before));
        path.quadTo(corner, corner + outgoing * (radius / after));
    }
    path.lineTo(route.last());
    return path;
}
}

QString BlueprintCanvas::nodeIcon(const QString &kind) {
    if (kind == "input") return "message-circle";
    if (kind == "dataset") return "database";
    if (kind == "model") return "layers";
    if (kind == "output") return "file-down";
    if (kind == "selector") return "search";
    if (kind == "extractor") return "layers";
    if (kind == "pricing") return "check";
    if (kind == "tool") return "settings";
    return "cpu";
}

QColor BlueprintCanvas::nodeAccent(const QString &kind) {
    if (kind == "selector") return QColor(Theme::accentLabel());
    if (kind == "extractor") return QColor(Theme::statusWarning());
    if (kind == "pricing" || kind == "dataset" || kind == "output") return QColor(Theme::statusSuccess());
    if (kind == "tool") return QColor(Theme::textSecondary());
    return QColor(Theme::comparisonSeries());
}

BlueprintCanvas::BlueprintCanvas(QWidget *parent) : QWidget(parent), m_edgePan(new QTimer(this)) {
    setObjectName("blueprintCanvas");
    setAccessibleName("Workflow graph");
    setAccessibleDescription("Drag the background to pan and drag a node to move it. Scroll to zoom. Use the arrow keys to pan, plus or minus to zoom, and Home to fit.");
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(240, 275);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    m_edgePan->setInterval(16);
    connect(m_edgePan, &QTimer::timeout, this, &BlueprintCanvas::panAtEdge);
}

void BlueprintCanvas::setGraph(const QJsonObject &graph, const QJsonObject &fixtures) {
    m_fixtures = fixtures;
    m_edges = graph["edges"].toArray();
    m_nodes.clear();
    m_order.clear();
    for (const auto &value : graph["nodes"].toArray()) {
        const auto node = value.toObject();
        const auto id = node["id"].toString();
        m_nodes.insert(id, node);
        m_order.append(id);
    }
    if (m_autoFit) fit();
    else update();
}

void BlueprintCanvas::setSelection(const QString &node, const QString &outputNode, const QString &outputPort) {
    m_selected = node;
    m_armedNode = outputNode;
    m_armedPort = outputPort;
    update();
}

QRectF BlueprintCanvas::nodeRect(const QJsonObject &node) const {
    return {node["x"].toDouble(), node["y"].toDouble(), nodeWidth,
            qreal(116 + portSpacing * qMax(node["in"].toArray().size(), node["out"].toArray().size()))};
}

QPointF BlueprintCanvas::portCenter(const QJsonObject &node, bool output, int index) const {
    const auto box = nodeRect(node);
    return {output ? box.right() : box.left(), box.top() + firstPort + portSpacing * index};
}

int BlueprintCanvas::portIndex(const QJsonObject &node, bool output, const QString &id) const {
    const auto ports = node[output ? "out" : "in"].toArray();
    for (int i = 0; i < ports.size(); ++i)
        if (ports.at(i).toObject()["id"].toString() == id) return i;
    return -1;
}

void BlueprintCanvas::fit() {
    QRectF extent;
    for (const auto &node : m_nodes) extent = extent.united(nodeRect(node).adjusted(-24, -34, 24, 28));
    if (extent.isEmpty()) extent = QRectF(0, 0, 680, 438);
    m_autoFit = true;
    m_zoom = qBound(minZoom, qMin((width() - 26) / extent.width(), (height() - 26) / extent.height()), qreal(1));
    m_origin = QRectF(rect()).center() - extent.center() * m_zoom;
    emit zoomChanged(qRound(m_zoom * 100));
    update();
}

void BlueprintCanvas::zoomAt(qreal factor, const QPointF &position) {
    if (m_nodes.isEmpty()) return;
    const QPointF anchored = graphPoint(position);
    m_autoFit = false;
    m_zoom = qBound(minZoom, m_zoom * factor, qreal(3));
    m_origin = position - anchored * m_zoom;
    emit zoomChanged(qRound(m_zoom * 100));
    update();
}

void BlueprintCanvas::zoomBy(qreal factor) { zoomAt(factor, QRectF(rect()).center()); }

void BlueprintCanvas::revealNode(const QString &id) {
    const auto node = m_nodes.value(id);
    if (node.isEmpty()) return;
    const auto box = nodeRect(node);
    if (!QRectF(rect()).adjusted(5, 5, -5, -5).contains(QRectF(box.topLeft() * m_zoom + m_origin, box.size() * m_zoom))) {
        m_autoFit = false;
        m_origin = QRectF(rect()).center() - box.center() * m_zoom;
        update();
    }
}

void BlueprintCanvas::resizeEvent(QResizeEvent *event) {
    QWidget::resizeEvent(event);
    if (m_autoFit) fit();
    else if (event->oldSize().isValid())
        m_origin += QPointF(event->size().width() - event->oldSize().width(),
                            event->size().height() - event->oldSize().height()) / 2;
}

void BlueprintCanvas::wheelEvent(QWheelEvent *event) {
    const qreal delta = event->pixelDelta().isNull() ? event->angleDelta().y() : event->pixelDelta().y();
    if (delta != 0 && !m_nodes.isEmpty()) {
        zoomAt(std::exp(delta * .0016), event->position());
        event->accept();
    } else event->ignore();
}

QPointF BlueprintCanvas::centerInGraph() const { return graphPoint(QRectF(rect()).center()); }

QString BlueprintCanvas::nodeAt(const QPointF &point) const {
    for (auto it = m_order.crbegin(); it != m_order.crend(); ++it) {
        if (nodeRect(m_nodes.value(*it)).adjusted(-8, 0, 8, 0).contains(point)) return *it;
    }
    return {};
}

void BlueprintCanvas::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) return;
    const auto point = graphPoint(event->position());
    const auto id = nodeAt(point);
    if (!id.isEmpty()) {
        const auto node = m_nodes.value(id);
        for (const bool output : {false, true}) {
            const auto ports = node[output ? "out" : "in"].toArray();
            for (int index = 0; index < ports.size(); ++index) {
                const auto center = portCenter(node, output, index);
                const QRectF hit(center.x() - 8, center.y() - 8, 16, 16);
                if (hit.contains(point)) {
                    emit portSelected(id, output, ports.at(index).toObject()["id"].toString());
                    return;
                }
            }
        }
        m_dragged = id;
        m_press = event->position();
        m_dragStart = point - nodeRect(node).topLeft();
        emit nodeSelected(id);
        return;
    }
    m_panning = true;
    m_press = event->position();
    m_dragStart = m_origin;
    setCursor(Qt::ClosedHandCursor);
    event->accept();
}

void BlueprintCanvas::mouseMoveEvent(QMouseEvent *event) {
    if (!event->buttons().testFlag(Qt::LeftButton)) return;
    if (m_panning) {
        if ((event->position() - m_press).manhattanLength() >= QApplication::startDragDistance())
            m_autoFit = false;
        if (!m_autoFit) {
            m_origin = m_dragStart + event->position() - m_press;
            update();
        }
    } else if (!m_dragged.isEmpty()) {
        if (!m_dragging && (event->position() - m_press).manhattanLength() < QApplication::startDragDistance()) return;
        m_dragging = true;
        m_autoFit = false;
        m_pointer = event->position();
        setCursor(Qt::ClosedHandCursor);
        moveNode();
        if (!m_edgePan->isActive()) m_edgePan->start();
    }
}

void BlueprintCanvas::moveNode() {
    auto &node = m_nodes[m_dragged];
    const auto position = (graphPoint(m_pointer) - m_dragStart).toPoint();
    node["x"] = position.x();
    node["y"] = position.y();
    update();
}

void BlueprintCanvas::panAtEdge() {
    constexpr qreal margin = 34;
    auto speed = [](qreal point, int span) {
        if (point < margin) return qMin(20.0, (margin - point) * .28);
        if (point > span - margin) return -qMin(20.0, (point - span + margin) * .28);
        return 0.0;
    };
    const QPointF shift(speed(m_pointer.x(), width()), speed(m_pointer.y(), height()));
    if (shift.isNull() || m_dragged.isEmpty()) return;
    m_origin += shift;
    moveNode();
}

void BlueprintCanvas::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) return;
    if (std::exchange(m_panning, false) &&
        (event->position() - m_press).manhattanLength() < QApplication::startDragDistance())
        emit nodeSelected({});
    m_edgePan->stop();
    if (!m_dragged.isEmpty()) {
        const auto id = std::exchange(m_dragged, {});
        if (std::exchange(m_dragging, false)) emit nodeMoved(id, nodeRect(m_nodes.value(id)).topLeft().toPoint());
    }
    unsetCursor();
}

void BlueprintCanvas::contextMenuEvent(QContextMenuEvent *event) {
    const auto point = event->reason() == QContextMenuEvent::Keyboard ? centerInGraph() : graphPoint(event->pos());
    const auto id = event->reason() == QContextMenuEvent::Keyboard ? m_selected : nodeAt(point);
    const auto global = event->reason() == QContextMenuEvent::Keyboard ? mapToGlobal(rect().center()) : event->globalPos();
    emit nodeMenuRequested(id, global, point);
    event->accept();
}

void BlueprintCanvas::keyPressEvent(QKeyEvent *event) {
    switch (event->key()) {
    case Qt::Key_Plus: case Qt::Key_Equal: zoomBy(1.2); return;
    case Qt::Key_Minus: zoomBy(1 / 1.2); return;
    case Qt::Key_Home: fit(); return;
    case Qt::Key_Delete: if (!m_selected.isEmpty()) emit deleteRequested(m_selected); return;
    case Qt::Key_Left: m_origin.rx() += 45; break;
    case Qt::Key_Right: m_origin.rx() -= 45; break;
    case Qt::Key_Up: m_origin.ry() += 45; break;
    case Qt::Key_Down: m_origin.ry() -= 45; break;
    default: QWidget::keyPressEvent(event); return;
    }
    m_autoFit = false;
    update();
}

void BlueprintCanvas::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    const QColor background(Theme::canvasBackground());
    painter.fillRect(rect(), background);
    QColor dot(Theme::textMuted());
    dot.setAlphaF(.24);
    painter.setPen(QPen(dot, 1));
    const qreal spacing = qMax(7.0, 17 * m_zoom);
    auto mod = [spacing](qreal offset) { return std::fmod(std::fmod(offset, spacing) + spacing, spacing); };
    for (qreal y = mod(m_origin.y()); y < height(); y += spacing)
        for (qreal x = mod(m_origin.x()); x < width(); x += spacing)
            painter.drawPoint(QPointF(x, y));
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(m_origin);
    painter.scale(m_zoom, m_zoom);
    drawConnections(painter);
    for (const auto &id : m_order) drawNode(painter, m_nodes.value(id));
}

void BlueprintCanvas::drawConnections(QPainter &painter) const {
    for (const auto &value : m_edges) {
        const auto edge = value.toObject();
        const auto fromId = edge["from"].toString();
        const auto toId = edge["to"].toString();
        const auto from = m_nodes.value(fromId);
        const auto to = m_nodes.value(toId);
        const auto outId = edge["out"].toString();
        const int outIndex = portIndex(from, true, outId);
        const int inIndex = portIndex(to, false, edge["in"].toString());
        if (outIndex < 0 || inIndex < 0) continue;
        const QPointF a = portCenter(from, true, outIndex);
        const QPointF b = portCenter(to, false, inIndex);
        const auto port = from["out"].toArray().at(outIndex).toObject();
        const bool highlighted = m_selected == fromId || m_selected == toId;
        auto color = portColor(port["type"].toString());
        color.setAlphaF(highlighted ? .9 : .62);
        painter.setPen(QPen(color, highlighted ? 2 : 1.5, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        QPainterPath path(a);
        if (std::abs(a.y() - b.y()) < 65 && b.x() > a.x() + 120) {
            const qreal rail = qMin(nodeRect(from).top(), nodeRect(to).top()) - 22;
            const qreal exitX = a.x() + 15;
            const qreal entryX = b.x() - 15;
            path = roundedCable({a, {exitX, a.y()}, {exitX, rail}, {entryX, rail}, {entryX, b.y()}, b});
        } else if (b.x() <= a.x() + 14) {
            const qreal rail = b.y() > a.y() + 68 ? (a.y() + b.y()) / 2 : qMax(a.y(), b.y()) + 70;
            const qreal exitX = a.x() + 19;
            const qreal entryX = b.x() - 19;
            path = roundedCable({a, {exitX, a.y()}, {exitX, rail}, {entryX, rail}, {entryX, b.y()}, b});
        } else {
            const qreal tangent = qMin(160.0, (b.x() - a.x()) * .65);
            path.cubicTo(a.x() + tangent, a.y(), b.x() - tangent, b.y(), b.x(), b.y());
        }
        painter.drawPath(path);
    }
}

void BlueprintCanvas::drawNode(QPainter &painter, const QJsonObject &node) const {
    const QRectF box = nodeRect(node);
    const auto code = node["operation"].toString(node["kind"].toString());
    const auto id = node["id"].toString();
    QJsonObject operation;
    for (const auto &entry : m_fixtures["operations"].toArray()) {
        const auto candidate = entry.toObject();
        if (candidate["id"].toString() == code) {
            operation = candidate;
            break;
        }
    }
    const auto kind = operation["kind"].toString(node["kind"].toString());
    QString kindLabel = kind;
    for (const auto &entry : m_fixtures["nodeTypes"].toArray()) {
        const auto candidate = entry.toObject();
        if (candidate["id"].toString() == kind) {
            kindLabel = candidate["label"].toString();
            break;
        }
    }
    const auto iconKind = code == "selector" || code == "extractor" ? code : kind;
    const auto contracts = m_fixtures["contracts"].toObject();
    const auto fill = QColor(Theme::surfaceFill());
    painter.setPen(Qt::NoPen);
    const QColor ink = nodeAccent(iconKind);
    const auto surface = id == m_selected ? QColor(Theme::surfaceFillHover()) : fill;
    painter.setBrush(surface);
    painter.drawRoundedRect(box, 9, 9);
    if (id == m_selected) {
        painter.setBrush(QColor(Theme::textSecondary()));
        painter.drawEllipse(QPointF(box.right() - 14, box.top() + 22), 2.5, 2.5);
    }
    const QRect glyphRect(qRound(box.left() + 11), qRound(box.top() + 10), 26, 26);
    auto tint = ink;
    tint.setAlphaF(.12);
    painter.setPen(Qt::NoPen);
    painter.setBrush(tint);
    painter.drawRoundedRect(glyphRect, 6, 6);
    Theme::coloredIcon(nodeIcon(iconKind), ink, 16).paint(&painter, glyphRect.adjusted(5, 5, -5, -5));
    QFont meta(Theme::bodyFont());
    meta.setPixelSize(11);
    painter.setFont(meta);
    painter.setPen(QColor(Theme::textMuted()));
    if (node["name"].toString() != kindLabel)
        painter.drawText(box.adjusted(45, 10, -22, 10 - box.height() + 26), Qt::AlignVCenter, kindLabel);
    QFont title(Theme::headingFont());
    title.setPixelSize(15);
    title.setWeight(QFont::DemiBold);
    painter.setFont(title);
    painter.setPen(QColor(Theme::textPrimary()));
    painter.drawText(QRectF(box.x() + 11, box.y() + 44, box.width() - 22, 19), Qt::AlignVCenter,
                     QFontMetrics(title).elidedText(node["name"].toString(), Qt::ElideRight, nodeWidth - 22));
    QString detail;
    if (kind == "model" && !node.contains("operation")) {
        detail = "Choose an adapter";
        for (const auto &value : m_fixtures["adapters"].toArray()) {
            const auto adapter = value.toObject();
            if (adapter["id"] == node["adapter"]) detail = adapter["name"].toString();
        }
    } else {
        detail = operation["label"].toString();
        const QString exampleSuffix = " (order example)";
        if (detail.endsWith(exampleSuffix)) detail.chop(exampleSuffix.size());
    }
    painter.setFont(meta);
    painter.setPen(QColor(Theme::textMuted()));
    painter.drawText(QRectF(box.x() + 11, box.y() + 63, box.width() - 22, 17), Qt::AlignVCenter,
                     QFontMetrics(meta).elidedText(detail, Qt::ElideRight, nodeWidth - 22));
    QFont mono(Theme::monoFont());
    mono.setPixelSize(10);
    painter.setFont(mono);
    for (const bool output : {false, true}) {
        const auto ports = node[output ? "out" : "in"].toArray();
        for (int index = 0; index < ports.size(); ++index) {
            const auto port = ports.at(index).toObject();
            const QPointF center = portCenter(node, output, index);
            const auto type = port["type"].toString();
            const auto tone = portColor(type);
            painter.setPen(QPen(tone, 1));
            painter.setBrush(output && id == m_armedNode && port["id"].toString() == m_armedPort ? tone : surface);
            painter.drawEllipse(center, 3.6, 3.6);
            painter.setPen(QColor(Theme::textSecondary()));
            const auto label = port["name"].toString(contracts.value(type).toObject()["label"].toString());
            const auto visibleLabel = QFontMetrics(mono).elidedText(label, Qt::ElideRight, 84);
            const qreal start = output ? center.x() - 92 : center.x() + 8;
            painter.drawText(QRectF(start, center.y() - 7, 84, 14),
                Qt::AlignVCenter | (output ? Qt::AlignRight : Qt::AlignLeft), visibleLabel);
        }
    }
}
