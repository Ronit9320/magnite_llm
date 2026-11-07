#pragma once
#include "theme.h"
#include <QObject>
#include <QSettings>
#include <QVariantAnimation>
#include <functional>

// One bounded transition per component; model values never depend on animation.
class MotionValue : public QObject {
public:
    MotionValue(QObject *parent, std::function<void(qreal)> changed)
        : QObject(parent), m_changed(std::move(changed)), m_animation(new QVariantAnimation(this)) {
        m_animation->setEasingCurve(QEasingCurve::OutCubic);
        connect(m_animation,&QVariantAnimation::valueChanged,this,[this](const QVariant &value) {
            if (reduced()) { snap(m_target); return; }
            m_value=value.toReal(); m_changed(m_value);
        });
    }
    static bool reduced() { return QSettings().value("appearance/studio3ReducedMotion",false).toBool(); }
    qreal value() const { return m_value; }
    bool running() const { return m_animation->state()==QAbstractAnimation::Running; }
    void snap(qreal value) { m_animation->stop(); m_value=m_target=value; m_changed(value); }
    void to(qreal target, bool animate=true, int duration=Theme::motionFastMs()) {
        if (!animate || reduced()) { snap(target); return; }
        if (qFuzzyCompare(m_target+1,target+1) && running()) return;
        m_animation->stop(); m_target=target;
        if (qFuzzyCompare(m_value+1,target+1)) { snap(target); return; }
        m_animation->setDuration(duration);
        m_animation->setStartValue(m_value); m_animation->setEndValue(target); m_animation->start();
    }
private:
    std::function<void(qreal)> m_changed;
    QVariantAnimation *m_animation;
    qreal m_value=0, m_target=0;
};
