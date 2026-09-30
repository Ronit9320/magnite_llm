#include "widgets/starfield.h"
#include "theme.h"

#include <QApplication>
#include <QPainter>
#include <QWindow>
#include <QPainterPath>
#include <cmath>

Starfield::Starfield(QWidget *parent) : QWidget(parent) {
    m_clock.start();
    m_timer.setInterval(33); // Smooth activity feedback without a continuously running render loop.
    connect(&m_timer, &QTimer::timeout, this, qOverload<>(&QWidget::update));
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState) { updateTimer(); });
}

void Starfield::setActivity(Activity activity) {
    m_activity = activity;
    setAccessibleName(activity == Training ? "Training in progress" : activity == Chat ? "Model responding" : "Magnite seal");
    updateTimer();
    update();
}

void Starfield::setMotionEnabled(bool enabled) {
    m_motion = enabled;
    updateTimer();
    update();
}

void Starfield::setStarsVisible(bool visible) {
    m_stars = visible;
    updateTimer();
    update();
}

void Starfield::updateTimer() {
    const bool animate = property("kineticGlyph").toBool() && m_motion && m_stars
        && (m_activity != Idle || !property("activityIndicator").toBool()) && isVisible()
        && qApp->applicationState() == Qt::ApplicationActive;
    if (animate) m_timer.start(); else m_timer.stop();
}

void Starfield::showEvent(QShowEvent *event) { QWidget::showEvent(event); updateTimer(); }
void Starfield::hideEvent(QHideEvent *event) { QWidget::hideEvent(event); updateTimer(); }

void Starfield::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (property("kineticGlyph").toBool()) {
        painter.translate(width()/2.0,height()/2.0);
        const qreal scale = qMin(width(),height())/210.0;
        painter.scale(scale,scale);
        const qreal seconds = m_motion && (m_activity != Idle || !property("activityIndicator").toBool())
            ? m_clock.elapsed()/1000.0 : 0;
        const QColor accent(Theme::accentPrimary());
        QColor quiet(Theme::textMuted()); quiet.setAlphaF(0.35);
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(quiet, 1));
        painter.drawEllipse(QPointF(0,0),99,99);
        painter.drawEllipse(QPointF(0,0),92,92);
        // Fine registration marks preserve the radial identity at larger sizes.
        painter.save();
        for (int i=0; i<48; ++i) {
            painter.setPen(QPen(i%4 == 0 ? accent : quiet, i%4 == 0 ? 1.5 : 0.8));
            painter.drawLine(QPointF(0,-92), QPointF(0,i%4 == 0 ? -87 : -89));
            painter.rotate(7.5);
        }
        painter.restore();
        painter.save();
        painter.rotate(seconds * (m_activity == Training ? 32 : 5));
        painter.setPen(Qt::NoPen);
        painter.setBrush(accent);
        for (int i=0;i<12;++i) {
            painter.drawRoundedRect(QRectF(31,-7,48,14),3,3);
            painter.rotate(30);
        }
        painter.restore();
        painter.setBrush(Qt::NoBrush);
        painter.setPen(QPen(accent,1.5));
        painter.drawEllipse(QPointF(0,0),27,27);
        // A small faceted core adds detail without changing the seal silhouette.
        QPainterPath core;
        core.moveTo(0,-16); core.lineTo(14,-8); core.lineTo(14,8);
        core.lineTo(0,16); core.lineTo(-14,8); core.lineTo(-14,-8); core.closeSubpath();
        painter.setBrush(QColor(Theme::glassFill()));
        painter.drawPath(core);
        painter.drawLine(QPointF(-14,-8),QPointF(0,0));
        painter.drawLine(QPointF(14,-8),QPointF(0,0));
        painter.drawLine(QPointF(0,16),QPointF(0,0));
        if (m_activity != Idle) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(accent,3,Qt::SolidLine,Qt::RoundCap));
            painter.drawArc(QRectF(-99,-99,198,198), int(-seconds*90*16), 85*16);
            if (m_activity == Chat) {
                painter.setPen(Qt::NoPen); painter.setBrush(accent);
                for (int i=0;i<3;++i) {
                    const qreal rise = m_motion ? std::sin(seconds*5-i*0.9)*3 : 0;
                    painter.drawEllipse(QPointF((i-1)*11, rise),3.5,3.5);
                }
            }
        }
        return;
    }
    painter.fillRect(rect(), QColor(Theme::bgVoid()));
    if (!m_stars) return;
    painter.setRenderHint(QPainter::Antialiasing);
    QColor grid(Theme::glassBorder()); grid.setAlphaF(0.18);
    painter.setPen(QPen(grid,1));
    for (int x=24;x<width();x+=32)
        for (int y=24;y<height();y+=32) {
            painter.drawPoint(x,y);
        }
}
