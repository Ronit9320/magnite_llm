#include "toggleswitch.h"
#include "theme.h"

#include <QEnterEvent>
#include <QPainter>
#include <QSettings>
#include <QStyle>
#include <QVariantAnimation>
#include <QtMath>

namespace {
constexpr qreal kTrackWidth = 42;
constexpr qreal kTrackHeight = 24;
constexpr qreal kThumbDiameter = 18;
constexpr int kGap = 10;
constexpr int kPadding = 4;
}

ToggleSwitch::ToggleSwitch(const QString &text, QWidget *parent)
    : QCheckBox(text,parent), m_animation(new QVariantAnimation(this)) {
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setSizePolicy(QSizePolicy::Preferred,QSizePolicy::Fixed);
    m_animation->setDuration(160);
    m_animation->setEasingCurve(QEasingCurve::InOutCubic);
    connect(m_animation,&QVariantAnimation::valueChanged,this,[this](const QVariant &value) {
        if (QSettings().value("appearance/studio3ReducedMotion",false).toBool()) {
            settle(); return;
        }
        m_position=value.toReal(); update();
    });
    connect(this,&QCheckBox::toggled,this,[this](bool checked) {
        m_animation->stop();
        if (!isVisible() || !isEnabled() || QSettings().value("appearance/studio3ReducedMotion",false).toBool()) {
            settle(); return;
        }
        m_animation->setStartValue(m_position);
        m_animation->setEndValue(checked ? 1.0 : 0.0);
        m_animation->start();
    });
}

QSize ToggleSwitch::sizeHint() const {
    const auto label=fontMetrics().size(Qt::TextShowMnemonic,text());
    return {qCeil(kTrackWidth)+2*kPadding+(text().isEmpty()?0:kGap+label.width()),
            qMax(qCeil(kTrackHeight),label.height())+2*kPadding};
}

bool ToggleSwitch::isAnimating() const { return m_animation->state()==QAbstractAnimation::Running; }
void ToggleSwitch::settle() {
    m_animation->stop(); m_position=isChecked()?1.0:0.0; update();
}
bool ToggleSwitch::hitButton(const QPoint &position) const { return rect().contains(position); }
void ToggleSwitch::enterEvent(QEnterEvent *event) { QCheckBox::enterEvent(event); update(); }
void ToggleSwitch::leaveEvent(QEvent *event) { QCheckBox::leaveEvent(event); update(); }
void ToggleSwitch::changeEvent(QEvent *event) {
    QCheckBox::changeEvent(event);
    if (event->type()==QEvent::EnabledChange) settle();
    if (event->type()==QEvent::FontChange || event->type()==QEvent::StyleChange || event->type()==QEvent::LayoutDirectionChange)
        updateGeometry();
    update();
}
void ToggleSwitch::hideEvent(QHideEvent *event) { settle(); QCheckBox::hideEvent(event); }

void ToggleSwitch::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const bool rtl=layoutDirection()==Qt::RightToLeft;
    const qreal x=rtl ? width()-kPadding-kTrackWidth : kPadding;
    const QRectF track(x,(height()-kTrackHeight)/2.0,kTrackWidth,kTrackHeight);
    const bool hover=isEnabled() && underMouse();
    auto blend=[this](const QColor &off,const QColor &on) {
        return QColor::fromRgbF(off.redF()+(on.redF()-off.redF())*m_position,
                               off.greenF()+(on.greenF()-off.greenF())*m_position,
                               off.blueF()+(on.blueF()-off.blueF())*m_position);
    };
    QColor fill=blend(QColor(Theme::surfaceBorder()),QColor(hover?Theme::accentHover():Theme::accentPrimary()));
    QColor edge=blend(QColor(Theme::textMuted()),QColor(Theme::accentPrimary()));
    if (!isEnabled()) { fill=QColor(Theme::insetBackground()); edge=QColor(Theme::surfaceBorder()); }
    if (hasFocus() && isEnabled()) {
        painter.setPen(QPen(QColor(Theme::accentLabel()),1.5)); painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(track.adjusted(-3,-3,3,3),15,15);
    }
    painter.setPen(QPen(edge,1)); painter.setBrush(fill);
    painter.drawRoundedRect(track,kTrackHeight/2,kTrackHeight/2);
    const qreal inset=(kTrackHeight-kThumbDiameter)/2;
    const qreal progress=rtl?1-m_position:m_position;
    const qreal travel=kTrackWidth-kThumbDiameter-2*inset;
    const QRectF thumb(track.left()+inset+travel*progress,track.top()+inset,kThumbDiameter,kThumbDiameter);
    QColor thumbColor(Theme::isDark()?Theme::textPrimary():Theme::surfaceFill());
    if (!isEnabled()) thumbColor=QColor(Theme::textMuted());
    painter.setPen(QPen(QColor(Theme::textSecondary()),0.6)); painter.setBrush(thumbColor);
    painter.drawEllipse(thumb);
    QRect labelRect=rect();
    if (rtl) labelRect.setRight(qFloor(track.left())-kGap);
    else labelRect.setLeft(qCeil(track.right())+kGap);
    const int mnemonic=style()->styleHint(QStyle::SH_UnderlineShortcut,nullptr,this)?Qt::TextShowMnemonic:Qt::TextHideMnemonic;
    painter.setPen(QColor(isEnabled()?Theme::textPrimary():Theme::textMuted()));
    painter.drawText(labelRect,Qt::AlignVCenter|(rtl?Qt::AlignRight:Qt::AlignLeft)|mnemonic,text());
}
