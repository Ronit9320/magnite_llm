#include "widgets/smoothscrollbar.h"

#include "theme.h"

#include <QEnterEvent>
#include <QEasingCurve>
#include <QMouseEvent>
#include <QPainter>
#include <QSettings>
#include <QStyle>
#include <QStyleOptionSlider>
#include <QVariantAnimation>

SmoothScrollBar::SmoothScrollBar(Qt::Orientation orientation, QWidget *parent) : QScrollBar(orientation, parent) {
    setProperty("smooth", true);   // console.qss makes the native handle transparent for these bars
    m_glide = new QVariantAnimation(this);
    m_glide->setDuration(kGlideMs);
    m_glide->setEasingCurve(QEasingCurve::InOutCubic);
    connect(m_glide, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
        m_swell = value.toDouble();
        update();
    });
}

bool SmoothScrollBar::isSwelling() const {
    return m_glide->state() == QAbstractAnimation::Running;
}

void SmoothScrollBar::retarget() {
    const double target = (m_hovering || isSliderDown()) ? 1.0 : 0.0;
    if (m_glide->endValue().toDouble() == target && (isSwelling() || m_swell == target)) return;
    m_glide->stop();
    if (QSettings().value("appearance/studio3ReducedMotion", false).toBool()) {
        m_swell = target;
        update();
        return;
    }
    m_glide->setStartValue(m_swell);
    m_glide->setEndValue(target);
    m_glide->start();
}

void SmoothScrollBar::enterEvent(QEnterEvent *event) {
    QScrollBar::enterEvent(event);
    m_hovering = true;
    retarget();
}

void SmoothScrollBar::leaveEvent(QEvent *event) {
    QScrollBar::leaveEvent(event);
    m_hovering = false;
    retarget();
}

void SmoothScrollBar::mouseReleaseEvent(QMouseEvent *event) {
    QScrollBar::mouseReleaseEvent(event);
    retarget();   // a drag that ends away from the strip lets the handle relax
}

void SmoothScrollBar::paintEvent(QPaintEvent *event) {
    QScrollBar::paintEvent(event);   // transparent track from the style sheet
    if (maximum() <= minimum()) return;
    QStyleOptionSlider option;
    initStyleOption(&option);
    const QRect handle = style()->subControlRect(QStyle::CC_ScrollBar, &option, QStyle::SC_ScrollBarSlider, this);
    if (!handle.isValid()) return;

    const double thickness = kRestThickness + (kHoverThickness - kRestThickness) * m_swell;
    QRectF pill;
    if (orientation() == Qt::Vertical)
        pill = QRectF(width() - kEdgeInset - thickness, handle.top(), thickness, handle.height());
    else
        pill = QRectF(handle.left(), height() - kEdgeInset - thickness, handle.width(), thickness);

    QColor color(Theme::accentPrimary());
    const QColor hover(Theme::accentHover());
    color = QColor::fromRgbF(color.redF() + (hover.redF() - color.redF()) * m_swell,
                             color.greenF() + (hover.greenF() - color.greenF()) * m_swell,
                             color.blueF() + (hover.blueF() - color.blueF()) * m_swell);
    if (isSliderDown()) color = QColor(Theme::accentActive());

    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(color);
    painter.drawRoundedRect(pill, thickness / 2, thickness / 2);
}
