#include "widgets/studiocanvas.h"
#include "theme.h"
#include "widgets/brandmark.h"

#include <QApplication>
#include <QPainter>
#include <cmath>

StudioCanvas::StudioCanvas(QWidget *parent) : QWidget(parent) {
    m_clock.start();
    m_timer.setInterval(33); // Smooth activity feedback without a continuously running render loop.
    connect(&m_timer, &QTimer::timeout, this, qOverload<>(&QWidget::update));
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState) { updateTimer(); });
}

void StudioCanvas::setActivity(Activity activity) {
    m_activity = activity;
    m_clock.restart();
    setAccessibleName(activity == Training ? "Training in progress" : activity == Chat ? "Model responding" : "Magnite seal");
    updateTimer();
    update();
}

void StudioCanvas::setMotionEnabled(bool enabled) {
    m_motion = enabled;
    updateTimer();
    update();
}

void StudioCanvas::setGridVisible(bool visible) {
    m_grid = visible;
    update();
}

void StudioCanvas::updateTimer() {
    const bool animate = property("brandMark").toBool() && m_motion
        && (m_activity != Idle || !property("activityIndicator").toBool()) && isVisible()
        && qApp->applicationState() == Qt::ApplicationActive;
    if (animate) {
        if (!m_playbackClock.isValid()) m_playbackClock.start();
        m_timer.start();
    } else {
        if (m_playbackClock.isValid()) {
            m_playbackMs += m_playbackClock.elapsed();
            m_playbackClock.invalidate();
        }
        m_timer.stop();
    }
}

void StudioCanvas::showEvent(QShowEvent *event) { QWidget::showEvent(event); updateTimer(); }
void StudioCanvas::hideEvent(QHideEvent *event) { QWidget::hideEvent(event); updateTimer(); }

void StudioCanvas::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    if (property("brandMark").toBool()) {
        const qreal seconds = m_motion && (m_activity != Idle || !property("activityIndicator").toBool())
            ? m_clock.elapsed()/1000.0 : 0;
        const qreal side = qMin(width(),height());
        const QRectF bounds((width()-side)/2.0,(height()-side)/2.0,side,side);
        const bool playLogo = m_motion
            && (m_activity != Idle || !property("activityIndicator").toBool());
        const qint64 elapsed = m_playbackMs
            + (m_playbackClock.isValid() ? m_playbackClock.elapsed() : 0);
        if (!playLogo || !m_logo.paint(painter, bounds, elapsed)) {
            BrandMark::paint(painter,bounds,QColor(Theme::accentPrimary()));
        }
        if (m_activity != Idle) {
            painter.setBrush(Qt::NoBrush);
            painter.setPen(QPen(QColor(Theme::accentPrimary()),qMax(1.5,side/90.0),Qt::SolidLine,Qt::RoundCap));
            painter.drawArc(bounds.adjusted(2,2,-2,-2),int(-std::fmod(seconds*20,360.0)*16),55*16);
        }
        return;
    }
    painter.fillRect(rect(), QColor(Theme::canvasBackground()));
    if (!m_grid) return;
    painter.setRenderHint(QPainter::Antialiasing);
    QColor grid(Theme::surfaceBorder()); grid.setAlphaF(0.18);
    painter.setPen(QPen(grid,1));
    for (int x=24;x<width();x+=32)
        for (int y=24;y<height();y+=32) {
            painter.drawPoint(x,y);
        }
}
