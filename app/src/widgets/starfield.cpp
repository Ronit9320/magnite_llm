#include "widgets/starfield.h"
#include "theme.h"

#include <QApplication>
#include <QPainter>
#include <QRandomGenerator>
#include <QWindow>
#include <cmath>

Starfield::Starfield(QWidget *parent) : QWidget(parent) {
    m_clock.start();
    m_timer.setInterval(50); // 20 fps is sufficient for a 48-second ambient loop.
    connect(&m_timer, &QTimer::timeout, this, qOverload<>(&QWidget::update));
    connect(qApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState) { updateTimer(); });
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
    const bool animate = m_motion && m_stars && isVisible()
        && qApp->applicationState() == Qt::ApplicationActive;
    if (animate) m_timer.start(); else m_timer.stop();
}

void Starfield::showEvent(QShowEvent *event) { QWidget::showEvent(event); updateTimer(); }
void Starfield::hideEvent(QHideEvent *event) { QWidget::hideEvent(event); updateTimer(); }

void Starfield::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor(Theme::bgVoid()));
    if (!m_stars) return;
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    QRandomGenerator rng(24);
    const double phase = m_motion ? (1.0-std::cos(m_clock.elapsed()*6.283185307/48000.0))/2.0 : 0;
    for (int i = 0; i < 145; ++i) {
        const double x = rng.generateDouble()*width();
        const double y = rng.generateDouble()*height();
        const double radius = 0.6+rng.generateDouble()*0.7;
        QColor color(Theme::textPrimary());
        color.setAlphaF(0.15+rng.generateDouble()*0.45);
        painter.setBrush(color);
        painter.drawEllipse(QPointF(x+8*phase,y-12*phase),radius,radius);
    }
}
