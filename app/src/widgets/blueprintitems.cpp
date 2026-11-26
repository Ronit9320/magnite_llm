#include "widgets/blueprintitems.h"

#include "panels/blueprintpreview.h"
#include "theme.h"
#include "widgets/blueprintstyle.h"

#include <QFontMetrics>
#include <QGraphicsSceneHoverEvent>
#include <QHash>
#include <QPainter>
#include <QPainterPath>
#include <QPainterPathStroker>
#include <QStyleOptionGraphicsItem>
#include <QVariantAnimation>
#include <QtMath>
#include <algorithm>

namespace {
constexpr qreal nodePadding = 8;
constexpr qreal shadowMargin = 18;
constexpr qreal shadowOffset = 6;
using BlueprintStyle::pinHitRadius;

QPointF portCenter(const QJsonObject &node, bool output, int index) {
    const QSizeF size = NodeItem::sizeFor(node);
    return {output ? size.width() : 0.0,
            BlueprintStyle::headerHeight + BlueprintStyle::pinRowHeight * (index + 0.5)};
}

QString previewLine(const QJsonObject &node) {
    const auto text = node["template"].toString().simplified();
    if (!text.isEmpty()) return text;
    const auto model = node["model"].toString();
    if (!model.isEmpty()) return BlueprintPreview::modelLabel(model);
    const auto sample = node["sample"];
    if (sample.isString() && !sample.toString().isEmpty()) return sample.toString().simplified();
    for (const auto &value : node["in"].toArray()) {
        const auto port = value.toObject();
        if (port["sample"].isString() && !port["sample"].toString().isEmpty())
            return port["sample"].toString().simplified();
    }
    return {};
}

// Black, blurred silhouettes keyed by node size; painted with the theme's shadow opacity.
const QPixmap &shadowFor(const QSize &size) {
    static QHash<QSize, QPixmap> cache;
    auto found = cache.find(size);
    if (found != cache.end()) return found.value();
    const int margin = qCeil(shadowMargin);
    QImage image(size.width() + 2 * margin, size.height() + 2 * margin, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);
    painter.drawRoundedRect(QRectF(QPointF(margin, margin), QSizeF(size)), BlueprintStyle::cornerRadius,
                            BlueprintStyle::cornerRadius);
    painter.end();
    BlueprintStyle::blurAlpha(image, 5);
    return cache.insert(size, QPixmap::fromImage(image)).value();
}
}  // namespace

// ---- PinItem ----

PinItem::PinItem(NodeItem *node, bool output, const QJsonObject &port)
    : QGraphicsItem(node), m_node(node), m_output(output), m_port(port) {
    setAcceptHoverEvents(true);
}

QString PinItem::nodeId() const { return m_node->id(); }

void PinItem::setHighlight(Highlight highlight) {
    if (m_highlight == highlight) return;
    m_highlight = highlight;
    update();
}

QRectF PinItem::boundingRect() const { return {-pinHitRadius, -pinHitRadius, 2 * pinHitRadius, 2 * pinHitRadius}; }

QPainterPath PinItem::shape() const {
    QPainterPath path;
    path.addEllipse(QPointF(), pinHitRadius, pinHitRadius);
    return path;
}

void PinItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *) {
    const QColor tone = BlueprintStyle::typeColor(portType());
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setOpacity(m_highlight == Highlight::Dimmed ? 0.3 : 1.0);
    const qreal radius = BlueprintStyle::pinRadius + (m_hover || m_highlight == Highlight::Compatible ? 1.5 : 0);
    if (m_highlight == Highlight::Compatible) {
        QColor halo = tone;
        halo.setAlphaF(0.35);
        painter->setPen(Qt::NoPen);
        painter->setBrush(halo);
        painter->drawEllipse(QPointF(), radius + 4, radius + 4);
    }
    painter->setPen(QPen(QColor(Theme::surfaceFill()), 1.5));
    painter->setBrush(tone);
    painter->drawEllipse(QPointF(), radius, radius);
}

void PinItem::hoverEnterEvent(QGraphicsSceneHoverEvent *) {
    m_hover = true;
    update();
}

void PinItem::hoverLeaveEvent(QGraphicsSceneHoverEvent *) {
    m_hover = false;
    update();
}

// ---- WireItem ----

WireItem::WireItem(const QJsonObject &edge, PinItem *from, PinItem *to)
    : m_edge(edge), m_from(from), m_to(to), m_dash(new QVariantAnimation) {
    setFlag(ItemIsSelectable);
    setZValue(0);
    m_dash->setStartValue(0.0);
    m_dash->setEndValue(8.0);
    m_dash->setDuration(600);
    m_dash->setLoopCount(-1);
    QObject::connect(m_dash, &QVariantAnimation::valueChanged, [this] { update(); });
    QPen wire(BlueprintStyle::typeColor(from->portType()), 2);
    wire.setCapStyle(Qt::RoundCap);
    setPen(wire);
    updatePath();
}

WireItem::~WireItem() { delete m_dash; }

QPainterPath WireItem::pathBetween(const QPointF &a, const QPointF &b) {
    QPainterPath path(a);
    if (qAbs(a.y() - b.y()) < 0.5) {
        path.lineTo(b);
    } else {
        // Backward wires (into an input left of the output) bend by the vertical gap, capped, so they stay near the nodes.
        const qreal reach = b.x() < a.x() ? qBound<qreal>(48, qAbs(b.y() - a.y()) * 0.5, 120)
                                          : qMax<qreal>(48, qAbs(b.x() - a.x()) * 0.5);
        path.cubicTo(a + QPointF(reach, 0), b - QPointF(reach, 0), b);
    }
    return path;
}

void WireItem::updatePath() {
    const auto path = pathBetween(m_from->anchor(), m_to->anchor());
    prepareGeometryChange();
    setPath(path);
}

void WireItem::setActive(bool active) {
    if (m_active == active) return;
    m_active = active;
    if (active) m_dash->start();
    else m_dash->stop();
    update();
}

QPainterPath WireItem::shape() const {
    QPainterPathStroker stroker;
    stroker.setWidth(12);
    stroker.setCapStyle(Qt::RoundCap);
    return stroker.createStroke(path());
}

QRectF WireItem::boundingRect() const { return shape().boundingRect().adjusted(-6, -6, 6, 6); }

void WireItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *) {
    painter->setRenderHint(QPainter::Antialiasing);
    QColor tone = pen().color();
    if (isSelected()) tone = tone.lighter(135);
    QColor glow = tone;
    glow.setAlphaF(isSelected() ? 0.30 : 0.16);
    QPen glowPen(glow, isSelected() ? 8 : 6);
    glowPen.setCapStyle(Qt::RoundCap);
    painter->setBrush(Qt::NoBrush);
    painter->setPen(glowPen);
    painter->drawPath(path());
    QPen line(tone, isSelected() ? 2.6 : 2);
    line.setCapStyle(Qt::RoundCap);
    if (m_active) {
        line.setDashPattern({3, 3});
        line.setDashOffset(m_dash->currentValue().toReal());
    }
    painter->setPen(line);
    painter->drawPath(path());
}

// ---- NodeItem ----

QSizeF NodeItem::sizeFor(const QJsonObject &node) {
    const int rows = qMax(node["in"].toArray().size(), node["out"].toArray().size());
    qreal height = BlueprintStyle::headerHeight + rows * BlueprintStyle::pinRowHeight + nodePadding;
    if (!previewLine(node).isEmpty()) height += BlueprintStyle::previewHeight;
    return {BlueprintStyle::nodeWidth, height};
}

NodeItem::NodeItem(const QJsonObject &node, const QJsonObject &operation, const BlueprintPreview &preview)
    : m_node(node), m_size(sizeFor(node)), m_preview(preview) {
    setFlags(ItemIsMovable | ItemIsSelectable | ItemSendsGeometryChanges);
    setAcceptHoverEvents(true);
    setZValue(1);
    setPos(node["x"].toDouble(), node["y"].toDouble());
    const auto kind = operation["kind"].toString(node["kind"].toString());
    m_family = BlueprintStyle::nodeFamily(kind);
    m_icon = BlueprintStyle::nodeIcon(kind);
    m_operationLabel = operation["label"].toString();
    if (m_operationLabel.isEmpty()) m_operationLabel = preview.nodeKindLabel(kind);
    m_previewText = previewLine(node);
    for (const bool output : {false, true}) {
        int row = 0;
        for (const auto &value : node[output ? "out" : "in"].toArray()) {
            auto *item = new PinItem(this, output, value.toObject());
            item->setPos(portCenter(node, output, row++));
            m_pins << item;
        }
    }
}

PinItem *NodeItem::pin(bool output, const QString &portId) const {
    for (auto *item : m_pins)
        if (item->isOutput() == output && item->portId() == portId) return item;
    return nullptr;
}

void NodeItem::setWarning(const QString &text) {
    if (m_warning == text) return;
    m_warning = text;
    setToolTip(text);
    update();
}

void NodeItem::setRunStatus(const QString &status) {
    if (m_runStatus == status) return;
    m_runStatus = status;
    update();
}

QRectF NodeItem::boundingRect() const {
    return QRectF(QPointF(), m_size).adjusted(-shadowMargin, -shadowMargin, shadowMargin, shadowMargin);
}

QPainterPath NodeItem::shape() const {
    QPainterPath path;
    path.addRoundedRect(QRectF(QPointF(), m_size), BlueprintStyle::cornerRadius, BlueprintStyle::cornerRadius);
    return path;
}

QVariant NodeItem::itemChange(GraphicsItemChange change, const QVariant &value) {
    if (change == ItemPositionHasChanged) emit moved();
    return QGraphicsObject::itemChange(change, value);
}

QRectF NodeItem::infoRect() const {
    qreal right = m_size.width() - 10;
    if (!m_warning.isEmpty()) right -= 20;
    if (!m_runStatus.isEmpty()) right -= 16;
    return QRectF(right - 15, (BlueprintStyle::headerHeight - 14) / 2, 14, 14);
}

void NodeItem::hoverEnterEvent(QGraphicsSceneHoverEvent *) {
    m_hover = true;
    update();
}

void NodeItem::hoverLeaveEvent(QGraphicsSceneHoverEvent *) {
    m_hover = false;
    update();
}

void NodeItem::paint(QPainter *painter, const QStyleOptionGraphicsItem *, QWidget *) {
    const QRectF box(QPointF(), m_size);
    const qreal radius = BlueprintStyle::cornerRadius;
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setRenderHint(QPainter::SmoothPixmapTransform);

    painter->save();
    painter->setOpacity(QColor(Theme::blueprintPanelShadow()).alphaF());
    painter->drawPixmap(QPointF(-shadowMargin, -shadowMargin + shadowOffset), shadowFor(m_size.toSize()));
    painter->restore();

    const QColor family = BlueprintStyle::familyColor(m_family);
    QColor outline = QColor(Theme::surfaceBorder());
    qreal outlineWidth = 1;
    if (isSelected()) {
        outline = QColor(Theme::accentPrimary());
        outlineWidth = 2;
    } else if (m_runStatus == "failed") outline = QColor(Theme::statusError());
    else if (m_runStatus == "passed") outline = QColor(Theme::statusSuccess());
    else if (m_runStatus == "running") outline = family;
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(Theme::surfaceFill()));
    painter->drawRoundedRect(box, radius, radius);

    // Header: family colour washed over the panel fill, clipped to the rounded top.
    painter->save();
    QPainterPath clip;
    clip.addRoundedRect(box, radius, radius);
    painter->setClipPath(clip);
    QColor tint = family;
    tint.setAlphaF(BlueprintStyle::headerTintAlpha);
    painter->fillRect(QRectF(0, 0, box.width(), BlueprintStyle::headerHeight), tint);
    painter->restore();

    painter->setPen(QPen(outline, outlineWidth));
    painter->setBrush(Qt::NoBrush);
    painter->drawRoundedRect(box.adjusted(outlineWidth / 2, outlineWidth / 2, -outlineWidth / 2, -outlineWidth / 2),
                             radius, radius);

    // Marks at the header's right edge: warning triangle, run status dot.
    qreal markRight = box.width() - 10;
    if (!m_warning.isEmpty()) {
        const QPointF c(markRight - 7, BlueprintStyle::headerHeight / 2);
        QPainterPath tri;
        tri.moveTo(c + QPointF(0, -7));
        tri.lineTo(c + QPointF(7, 6));
        tri.lineTo(c + QPointF(-7, 6));
        tri.closeSubpath();
        painter->setPen(Qt::NoPen);
        painter->setBrush(QColor(Theme::statusWarning()));
        painter->drawPath(tri);
        painter->setPen(QPen(QColor(Theme::surfaceFill()), 1.6, Qt::SolidLine, Qt::RoundCap));
        painter->drawLine(c + QPointF(0, -2), c + QPointF(0, 2));
        painter->drawPoint(c + QPointF(0, 4.2));
        markRight -= 20;
    }
    if (!m_runStatus.isEmpty()) {
        QColor dot = m_runStatus == "passed" ? QColor(Theme::statusSuccess())
                     : m_runStatus == "failed" ? QColor(Theme::statusError())
                     : m_runStatus == "waiting" ? QColor(Theme::statusWarning())
                                                : QColor(Theme::accentPrimary());
        painter->setPen(Qt::NoPen);
        painter->setBrush(dot);
        painter->drawEllipse(QPointF(markRight - 4, BlueprintStyle::headerHeight / 2), 4, 4);
        markRight -= 16;
    }

    if (infoShown()) {
        Theme::coloredIcon("info", QColor(Theme::textSecondary()), 14).paint(painter, infoRect().toRect());
        markRight -= 20;
    }

    const QRectF glyph(10, (BlueprintStyle::headerHeight - 16) / 2, 16, 16);
    Theme::coloredIcon(m_icon, family, 16).paint(painter, glyph.toRect());
    const qreal textLeft = 34, textWidth = markRight - textLeft;
    QFont title(Theme::headingFont());
    title.setPixelSize(13);
    title.setWeight(QFont::Medium);
    painter->setFont(title);
    painter->setPen(QColor(Theme::textPrimary()));
    const auto name = m_node["name"].toString();
    painter->drawText(QRectF(textLeft, 3, textWidth, 17), Qt::AlignVCenter,
                      QFontMetrics(title).elidedText(name, Qt::ElideRight, qFloor(textWidth)));
    QFont meta(Theme::bodyFont());
    meta.setPixelSize(11);
    painter->setFont(meta);
    painter->setPen(QColor(Theme::textMuted()));
    painter->drawText(QRectF(textLeft, 19, textWidth, 14), Qt::AlignVCenter,
                      QFontMetrics(meta).elidedText(m_operationLabel, Qt::ElideRight, qFloor(textWidth)));

    // Port labels.
    painter->setPen(QColor(Theme::textSecondary()));
    const QFontMetrics metrics(meta);
    const qreal labelWidth = (box.width() - 36) / 2;
    int rows[2] = {0, 0};
    for (auto *item : m_pins) {
        const int row = rows[item->isOutput()]++;
        const qreal y = BlueprintStyle::headerHeight + row * BlueprintStyle::pinRowHeight;
        const auto label = metrics.elidedText(m_preview.portLabel(item->port()), Qt::ElideRight, qFloor(labelWidth));
        const QRectF at = item->isOutput() ? QRectF(box.width() - 12 - labelWidth, y, labelWidth, BlueprintStyle::pinRowHeight)
                                           : QRectF(12, y, labelWidth, BlueprintStyle::pinRowHeight);
        painter->drawText(at, (item->isOutput() ? Qt::AlignRight : Qt::AlignLeft) | Qt::AlignVCenter, label);
    }

    if (!m_previewText.isEmpty()) {
        const int count = qMax(rows[0], rows[1]);
        const QRectF at(12, BlueprintStyle::headerHeight + count * BlueprintStyle::pinRowHeight,
                        box.width() - 24, BlueprintStyle::previewHeight);
        painter->setPen(QColor(Theme::textMuted()));
        painter->drawText(at, Qt::AlignVCenter,
                          metrics.elidedText(m_previewText, Qt::ElideRight, qFloor(at.width())));
    }
}
