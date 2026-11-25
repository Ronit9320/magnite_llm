#include "widgets/floatingpanel.h"

#include "theme.h"

#include <QImage>
#include <QPainter>
#include <QVector>

namespace {
constexpr int blurRadius = 8;
constexpr int shadowOffset = 4;

// Blurs the alpha of a black image in place with three box-blur passes.
void blurAlpha(QImage &image, int radius) {
    const int w = image.width(), h = image.height();
    QVector<quint8> a(w * h), b(w * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) a[y * w + x] = qAlpha(image.pixel(x, y));
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y) {
            int sum = 0;
            for (int x = -radius; x <= radius; ++x) sum += a[y * w + qBound(0, x, w - 1)];
            for (int x = 0; x < w; ++x) {
                b[y * w + x] = sum / (2 * radius + 1);
                sum += a[y * w + qMin(w - 1, x + radius + 1)] - a[y * w + qMax(0, x - radius)];
            }
        }
        for (int x = 0; x < w; ++x) {
            int sum = 0;
            for (int y = -radius; y <= radius; ++y) sum += b[qBound(0, y, h - 1) * w + x];
            for (int y = 0; y < h; ++y) {
                a[y * w + x] = sum / (2 * radius + 1);
                sum += b[qMin(h - 1, y + radius + 1) * w + x] - b[qMax(0, y - radius) * w + x];
            }
        }
    }
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) image.setPixel(x, y, qRgba(0, 0, 0, a[y * w + x]));
}
}  // namespace

int FloatingPanel::shadowMargin() { return 2 * blurRadius; }

FloatingPanel::FloatingPanel(QWidget *parent) : QFrame(parent) {
    setObjectName("blueprintFloating");
    setFrameShape(QFrame::NoFrame);
    const int margin = shadowMargin() + padding;
    setContentsMargins(margin, margin, margin, margin);
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
    if (!image.isNull()) blurAlpha(image, blurRadius / 2);
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
