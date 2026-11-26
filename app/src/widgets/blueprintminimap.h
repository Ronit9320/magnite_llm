#pragma once

#include <QMouseEvent>
#include <QTransform>
#include <QWheelEvent>
#include <QWidget>
#include <optional>

class BlueprintView;

// A small overview of the graph: node rectangles in their family colour and the area the view shows.
// Clicking or dragging centres the view on that point.
class BlueprintMinimap : public QWidget {
    Q_OBJECT
public:
    explicit BlueprintMinimap(BlueprintView *view);
    static constexpr int width = 180;
    static constexpr int height = 112;

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override { event->accept(); }

private:
    // Scene rectangle shown by the minimap (nodes and the visible area, padded) and its fit into the widget.
    QRectF sceneExtent() const;
    QTransform toWidget() const;
    void centreAt(const QPointF &widgetPosition);

    BlueprintView *m_view;
    std::optional<QTransform> m_frozen;  // the mapping while dragging, so the point under the cursor stays put
};
