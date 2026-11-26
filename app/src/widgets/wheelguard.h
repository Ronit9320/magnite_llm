#pragma once

#include <QObject>

// Stops the mouse wheel from changing a dropdown, spin box or slider that does not have focus:
// the turn scrolls the surrounding form instead, so values never change mid-scroll.
// Installed once on the application.
class WheelGuard : public QObject {
public:
    using QObject::QObject;
    static void install();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
};
