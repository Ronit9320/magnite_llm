#pragma once
#include "theme.h"
#include <QSpinBox>
#include <QDoubleSpinBox>
#include <QPainter>
#include <QIcon>
#include <QStyleOptionSpinBox>

namespace SpinControls {
inline void arrows(QAbstractSpinBox *widget,const QStyleOptionSpinBox &option) {
    QPainter painter(widget);
    for (const auto control : {QStyle::SC_SpinBoxUp,QStyle::SC_SpinBoxDown}) {
        const auto area=widget->style()->subControlRect(QStyle::CC_SpinBox,&option,control,widget);
        const auto step=control==QStyle::SC_SpinBoxUp?QAbstractSpinBox::StepUpEnabled:QAbstractSpinBox::StepDownEnabled;
        const bool enabled=widget->isEnabled() && !widget->isReadOnly() && option.stepEnabled.testFlag(step);
        Theme::coloredIcon(control==QStyle::SC_SpinBoxUp?"chevron-up":"chevron-down",
                           QColor(enabled?Theme::textSecondary():Theme::textMuted()),10)
            .paint(&painter,QRect(area.center().x()-5,area.center().y()-5,10,10));
    }
}
}
class SpinBox : public QSpinBox {
    Q_OBJECT
public:
    explicit SpinBox(QWidget *parent=nullptr) : QSpinBox(parent) {}
protected:
    void paintEvent(QPaintEvent *event) override { QSpinBox::paintEvent(event); QStyleOptionSpinBox option; initStyleOption(&option); SpinControls::arrows(this,option); }
};
class DoubleSpinBox : public QDoubleSpinBox {
    Q_OBJECT
public:
    explicit DoubleSpinBox(QWidget *parent=nullptr) : QDoubleSpinBox(parent) {}
protected:
    void paintEvent(QPaintEvent *event) override { QDoubleSpinBox::paintEvent(event); QStyleOptionSpinBox option; initStyleOption(&option); SpinControls::arrows(this,option); }
};
