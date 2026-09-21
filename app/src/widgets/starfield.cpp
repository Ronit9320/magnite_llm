#include "widgets/starfield.h"
#include "theme.h"

#include <QApplication>
#include <QPainter>
#include <QWindow>
#include <QRadialGradient>
#include <cmath>

Starfield::Starfield(QWidget *parent) : QWidget(parent) {
    m_clock.start();
    m_timer.setInterval(50); // 20 fps is sufficient for the slow ambient loop.
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
    const double phase = m_motion ? std::sin(m_clock.elapsed()*6.283185307/32000.0) : 0;
    auto glow = [&](QPointF center, qreal radius, const QString &token, qreal opacity) {
        QColor core(token); core.setAlphaF(opacity);
        QColor edge(core); edge.setAlpha(0);
        QRadialGradient gradient(center,radius);
        gradient.setColorAt(0,core); gradient.setColorAt(1,edge);
        painter.fillRect(rect(),gradient);
    };
    glow(QPointF(width()*0.83 + phase*45, height()*0.12),width()*0.6,Theme::accentPrimary(),0.10);
    glow(QPointF(width()*0.15, height()*0.9 + phase*30),width()*0.5,Theme::accentBlue(),0.065);
    QColor grid(Theme::glassBorder()); grid.setAlphaF(0.24);
    painter.setBrush(grid);
    for (int x=24;x<width();x+=32)
        for (int y=24;y<height();y+=32)
            painter.drawEllipse(QPointF(x,y),0.6,0.6);
}
