#include "widgets/starfield.h"
#include "theme.h"

#include <QApplication>
#include <QPainter>
#include <QWindow>

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
    const bool animate = property("kineticGlyph").toBool() && m_motion && m_stars && isVisible()
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
        painter.setPen(QPen(QColor(Theme::accentPrimary()),1));
        painter.drawEllipse(QPointF(0,0),99,99);
        painter.rotate(m_motion ? m_clock.elapsed()/180.0 : 0);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(Theme::accentPrimary()));
        for (int i=0;i<12;++i) {
            painter.drawRoundedRect(QRectF(24,-10,63,20),2,2);
            painter.rotate(30);
        }
        painter.setBrush(QColor(Theme::glassFill()));
        painter.drawEllipse(QPointF(0,0),24,24);
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
