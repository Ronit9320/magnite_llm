#pragma once

#include <QScrollBar>

class QVariantAnimation;

// A scrollbar whose slim handle glides wider while the pointer is over it, instead of snapping.
// The 12px strip is the hover and drag target; the handle is painted here (the QSS handle is
// transparent for [smooth="true"] bars), so the growth can be animated.
class SmoothScrollBar : public QScrollBar {
    Q_OBJECT
public:
    static constexpr int kRestThickness = 4;
    static constexpr int kHoverThickness = 8;
    static constexpr int kEdgeInset = 2;
    static constexpr int kGlideMs = 180;

    explicit SmoothScrollBar(Qt::Orientation orientation, QWidget *parent = nullptr);

    // 0 = slim at rest, 1 = fully swollen.
    double swell() const { return m_swell; }
    bool isSwelling() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void retarget();

    QVariantAnimation *m_glide;
    double m_swell = 0.0;
    bool m_hovering = false;
};
