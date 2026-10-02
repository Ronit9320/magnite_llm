#include "widgets/clickfeedback.h"
#include "theme.h"
#include <QApplication>
#include <QAbstractButton>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QPushButton>
#include <QPainterPath>
#include <QPainter>
#include <QSettings>
#include <QVariantAnimation>
#include <cmath>

namespace {
class Ripple : public QWidget {
public:
    Ripple(QAbstractButton *button, QPointF origin) : QWidget(button), m_origin(origin) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        setGeometry(button->rect());
        auto *animation = new QVariantAnimation(this);
        const bool sidebar = button->objectName() == "navBtn";
        animation->setDuration(sidebar ? 600 : 320);
        animation->setStartValue(0.0);
        animation->setEndValue(1.0);
        animation->setEasingCurve(sidebar ? QEasingCurve::InOutSine : QEasingCurve::OutCubic);
        connect(animation, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            m_progress = value.toReal(); update();
        });
        connect(animation, &QVariantAnimation::finished, this, &QObject::deleteLater);
        show(); raise(); animation->start();
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QPainterPath clip;
        clip.addRoundedRect(QRectF(rect()), Theme::cornerRadiusSmall(), Theme::cornerRadiusSmall());
        p.setClipPath(clip);
        QColor color(Theme::textPrimary());
        color.setAlphaF(0.14 * (1.0 - m_progress));
        p.setPen(Qt::NoPen); p.setBrush(color);
        const qreal radius = std::hypot(width(), height()) * m_progress;
        p.drawEllipse(m_origin, radius, radius);
    }
private:
    QPointF m_origin;
    qreal m_progress = 0;
};
}

ClickFeedback::ClickFeedback(QObject *parent) : QObject(parent) { qApp->installEventFilter(this); }

bool ClickFeedback::eventFilter(QObject *watched, QEvent *event) {
    auto *button = qobject_cast<QPushButton *>(watched);
    if (button && button->isEnabled() && button->isVisible()
        && !QSettings().value("appearance/studio3ReducedMotion", false).toBool()) {
        if (event->type() == QEvent::MouseButtonPress) {
            auto *mouse = static_cast<QMouseEvent *>(event);
            if (mouse->button() == Qt::LeftButton) new Ripple(button, mouse->position());
        } else if (event->type() == QEvent::KeyPress) {
            auto *key = static_cast<QKeyEvent *>(event);
            if (!key->isAutoRepeat() && (key->key() == Qt::Key_Space || key->key() == Qt::Key_Return))
                new Ripple(button, button->rect().center());
        }
    }
    return QObject::eventFilter(watched, event);
}
