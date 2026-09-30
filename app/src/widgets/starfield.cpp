#include "widgets/starfield.h"
#include "theme.h"
#include "widgets/brandmark.h"

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
    m_clock.restart();
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
        const qreal seconds = m_motion && (m_activity != Idle || !property("activityIndicator").toBool())
            ? m_clock.elapsed()/1000.0 : 0;
        const qreal angle = seconds * (m_activity == Training ? 12 : m_activity == Chat ? 6 : 1.5);
        const qreal side = qMin(width(),height());
        const QRectF bounds((width()-side)/2.0,(height()-side)/2.0,side,side);
        BrandMark::paint(painter,bounds,QColor(Theme::accentPrimary()),angle);
        if (m_activity != Idle) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor(Theme::accentPrimary()),qMax(1.5,side/90.0),Qt::SolidLine,Qt::RoundCap));
            painter.drawArc(bounds.adjusted(2,2,-2,-2),int(-std::fmod(seconds*80,360.0)*16),55*16);
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
