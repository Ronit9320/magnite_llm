#pragma once

#include <QGraphicsObject>
#include <QGraphicsPathItem>
#include <QJsonObject>
#include <QPixmap>
#include <QHash>
#include <QList>
#include <QSizeF>
#include <QString>

class BlueprintPreview;
class NodeItem;
class QVariantAnimation;

// A port dot on a node's left (input) or right (output) edge. Its origin is the pin centre.
class PinItem : public QGraphicsItem {
public:
    enum class Highlight { None, Compatible, Dimmed };

    PinItem(NodeItem *node, bool output, const QJsonObject &port);

    QString nodeId() const;
    QString portId() const { return m_port["id"].toString(); }
    bool isOutput() const { return m_output; }
    QString portType() const { return m_port["type"].toString(); }
    QPointF anchor() const { return mapToScene(QPointF()); }
    void setHighlight(Highlight highlight);
    QJsonObject port() const { return m_port; }
    Highlight highlight() const { return m_highlight; }

    QRectF boundingRect() const override;
    QPainterPath shape() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

protected:
    void hoverEnterEvent(QGraphicsSceneHoverEvent *event) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent *event) override;

private:
    NodeItem *m_node;
    bool m_output;
    QJsonObject m_port;
    Highlight m_highlight = Highlight::None;
    bool m_hover = false;
};

// A connection from an output pin to an input pin, drawn as a glowing cubic curve.
class WireItem : public QGraphicsPathItem {
public:
    WireItem(const QJsonObject &edge, PinItem *from, PinItem *to);
    ~WireItem() override;

    QJsonObject edge() const { return m_edge; }
    PinItem *from() const { return m_from; }
    PinItem *to() const { return m_to; }
    void updatePath();
    static QPainterPath pathBetween(const QPointF &from, const QPointF &to);
    void setActive(bool active);
    bool active() const { return m_active; }

    QPainterPath shape() const override;
    QRectF boundingRect() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

private:
    QJsonObject m_edge;
    PinItem *m_from;
    PinItem *m_to;
    bool m_active = false;
    QVariantAnimation *m_dash;
    mutable QPainterPath m_shapeFor;
    mutable QPainterPath m_shape;
    mutable QRectF m_bounds;
};

// One node: header tinted by family, input/output pins, and an optional one-line preview.
class NodeItem : public QGraphicsObject {
    Q_OBJECT
public:
    NodeItem(const QJsonObject &node, const QJsonObject &operation, const BlueprintPreview &preview);

    QString id() const { return m_node["id"].toString(); }
    QJsonObject node() const { return m_node; }
    QString family() const { return m_family; }
    PinItem *pin(bool output, const QString &portId) const;
    QList<PinItem *> pins() const { return m_pins; }
    void setWarning(const QString &text);
    QString warning() const { return m_warning; }
    void setRunStatus(const QString &status);
    QString runStatus() const { return m_runStatus; }
    static QSizeF sizeFor(const QJsonObject &node);
    // The ⓘ that opens the node's details, in item coordinates; shown on hover or selection.
    QRectF infoRect() const;
    bool infoShown() const { return m_hover || isSelected(); }

    QRectF boundingRect() const override;
    QPainterPath shape() const override;
    void paint(QPainter *painter, const QStyleOptionGraphicsItem *option, QWidget *widget) override;

signals:
    void moved();

protected:
    QVariant itemChange(GraphicsItemChange change, const QVariant &value) override;
    void hoverEnterEvent(QGraphicsSceneHoverEvent *event) override;
    void hoverLeaveEvent(QGraphicsSceneHoverEvent *event) override;

private:
    QJsonObject m_node;
    QString m_operationLabel;
    QString m_icon;
    QString m_family;
    QString m_previewText;
    QString m_warning;
    QString m_runStatus;
    QList<PinItem *> m_pins;
    QSizeF m_size;
    bool m_hover = false;
    mutable QHash<QString, QPixmap> m_glyphs;
    QPixmap glyph(const QString &icon, const QColor &color, int size) const;
    const BlueprintPreview &m_preview;
};
