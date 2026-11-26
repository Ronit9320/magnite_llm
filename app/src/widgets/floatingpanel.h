#pragma once

#include <QFrame>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPixmap>

// A borderless panel that floats over the Blueprints canvas: panel fill, rounded corners and a soft
// shadow painted from a pixmap that is rebuilt only when the panel is resized.
class FloatingPanel : public QFrame {
    Q_OBJECT
public:
    explicit FloatingPanel(QWidget *parent = nullptr);

    // Space around the panel body reserved for the shadow; contentsMargins include it.
    static int shadowMargin();
    // Padding between the panel body and its contents.
    static constexpr int padding = 6;
    static constexpr int radius = 12;

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    // The style sheet resets contents margins when it polishes a frame; the shadow needs them back.
    bool event(QEvent *event) override;
    // The canvas underneath must not see clicks or wheel turns on a panel.
    void mousePressEvent(QMouseEvent *event) override { event->accept(); }
    void mouseReleaseEvent(QMouseEvent *event) override { event->accept(); }
    void mouseMoveEvent(QMouseEvent *event) override { event->accept(); }
    void mouseDoubleClickEvent(QMouseEvent *event) override { event->accept(); }
    void wheelEvent(QWheelEvent *event) override { event->accept(); }

private:
    void rebuildShadow();

    QPixmap m_shadow;  // black silhouette, drawn with the theme's shadow opacity
};
