#include "widgets/themechoice.h"
#include "theme.h"
#include <QPainter>
#include <QPainterPath>
ThemeChoice::ThemeChoice(const QString& mode, QWidget* parent) : Button(parent), m_mode(mode) {
    setProperty("themeChoice", true);
    setText(mode == "light" ? "Light" : mode == "dark" ? "Dark" : "System");
    setAccessibleName(text() + " color theme");
    setCheckable(true);
    setMinimumSize(118, 133);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}
void ThemeChoice::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const QRectF outer = QRectF(rect()).adjusted(2, 2, -2, -2);
    painter.setPen(QPen(QColor(Theme::surfaceBorder()), 1));
    painter.setBrush(QColor(underMouse() ? Theme::surfaceFillHover() : Theme::surfaceFill()));
    painter.drawRoundedRect(outer, 8, 8);
    const QRectF preview = outer.adjusted(9, 9, -9, -39);
    painter.save();
    QPainterPath clip;
    clip.addRoundedRect(preview, 4, 4);
    painter.setClipPath(clip);
    painter.fillRect(preview,
                     QColor(m_mode == "dark" ? Theme::previewGraphite() : Theme::previewPaper()));
    if (m_mode == "system")
        painter.fillRect(
            QRectF(preview.center().x(), preview.top(), preview.width() / 2, preview.height()),
            QColor(Theme::previewGraphite()));
    QColor side(Theme::surfaceBorder());
    side.setAlpha(90);
    painter.fillRect(QRectF(preview.left(), preview.top(), preview.width() * .24, preview.height()),
                     side);
    painter.setPen(QPen(QColor(Theme::textMuted()), 2));
    for (int i = 0; i < 4; ++i) {
        painter.drawLine(
            QPointF(preview.left() + 6, preview.top() + 15 + i * 10),
            QPointF(preview.left() + preview.width() * .18, preview.top() + 15 + i * 10));
    }
    painter.setPen(QPen(QColor(Theme::accentPrimary()), 3));
    painter.drawLine(QPointF(preview.left() + 6, preview.top() + 10),
                     QPointF(preview.left() + preview.width() * .16, preview.top() + 10));
    painter.setPen(QPen(QColor(Theme::textMuted()), 3));
    for (int i = 0; i < 2; ++i)
        painter.drawLine(
            QPointF(preview.left() + preview.width() * .34, preview.top() + 26 + i * 9),
            QPointF(preview.right() - 14 - i * 8, preview.top() + 26 + i * 9));
    painter.setPen(QPen(QColor(Theme::surfaceBorder()), 1));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(preview.left() + preview.width() * .34, preview.bottom() - 25,
                                   preview.width() * .55, 13),
                            3, 3);
    painter.restore();
    painter.setPen(QColor(Theme::textPrimary()));
    painter.setFont(font());
    painter.drawText(QRectF(outer.left() + 12, outer.bottom() - 33, outer.width() - 30, 28),
                     Qt::AlignLeft | Qt::AlignVCenter, text());
    painter.setBrush(isChecked() ? QColor(Theme::accentPrimary()) : QColor(Theme::surfaceFill()));
    painter.setPen(QPen(QColor(isChecked() ? Theme::accentPrimary() : Theme::surfaceBorder()), 1));
    painter.drawEllipse(QPointF(outer.right() - 17, outer.bottom() - 19), 5, 5);
    if (isChecked() || hasFocus()) {
        painter.setBrush(Qt::NoBrush);
        painter.setPen(
            QPen(QColor(isChecked() ? Theme::accentPrimary() : Theme::surfaceBorder()), 2));
        painter.drawRoundedRect(outer, 8, 8);
    }
}
