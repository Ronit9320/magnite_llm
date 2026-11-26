#include "widgets/floatingpanel.h"

#include <QEvent>

#include "theme.h"
#include "widgets/blueprintstyle.h"

#include <QImage>
#include <QPainter>
#include <QVector>

namespace {
constexpr int blurRadius = 8;
constexpr int shadowOffset = 4;
}  // namespace

int FloatingPanel::shadowMargin() { return 2 * blurRadius; }

FloatingPanel::FloatingPanel(QWidget *parent) : QFrame(parent) {
    setObjectName("blueprintFloating");
    setFrameShape(QFrame::NoFrame);
    const int margin = shadowMargin() + padding;
    setContentsMargins(margin, margin, margin, margin);
}

bool FloatingPanel::event(QEvent *event) {
    const bool handled = QFrame::event(event);
    if (event->type() == QEvent::Polish || event->type() == QEvent::StyleChange) {
        const int margin = shadowMargin() + padding;
        if (contentsMargins() != QMargins(margin, margin, margin, margin)) setContentsMargins(margin, margin, margin, margin);
    }
    return handled;
}

void FloatingPanel::resizeEvent(QResizeEvent *event) {
    QFrame::resizeEvent(event);
    rebuildShadow();
}

void FloatingPanel::rebuildShadow() {
    QImage image(size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    const int margin = shadowMargin();
    QPainter painter(&image);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    painter.setBrush(Qt::black);
    painter.drawRoundedRect(QRectF(rect()).adjusted(margin, margin + shadowOffset, -margin, -margin + shadowOffset),
                            radius, radius);
    painter.end();
    if (!image.isNull()) BlueprintStyle::blurAlpha(image, blurRadius / 2);
    m_shadow = QPixmap::fromImage(image);
}

void FloatingPanel::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (m_shadow.size() != size()) rebuildShadow();
    painter.setOpacity(QColor(Theme::blueprintPanelShadow()).alphaF());
    painter.drawPixmap(0, 0, m_shadow);
    painter.setOpacity(1);
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(Theme::blueprintPanelFill()));
    const int margin = shadowMargin();
    painter.drawRoundedRect(QRectF(rect()).adjusted(margin, margin, -margin, -margin), radius, radius);
}
