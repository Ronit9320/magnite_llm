#include "widgets/wheelguard.h"

#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QScrollBar>
#include <QWheelEvent>

void WheelGuard::install() {
    static WheelGuard *guard = nullptr;
    if (guard || !qApp) return;
    guard = new WheelGuard(qApp);
    qApp->installEventFilter(guard);
}

bool WheelGuard::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() != QEvent::Wheel || !watched->isWidgetType()) return false;
    auto *widget = static_cast<QWidget *>(watched);
    // The value widget may be the target itself or the line edit inside a spin box or combo.
    QWidget *control = nullptr;
    for (auto *w = widget; w && !control; w = w->parentWidget()) {
        if (qobject_cast<QScrollBar *>(w)) return false;  // scroll bars always take the wheel
        if (qobject_cast<QComboBox *>(w) || qobject_cast<QAbstractSpinBox *>(w) || qobject_cast<QAbstractSlider *>(w))
            control = w;
        if (w->isWindow()) break;
    }
    if (!control || control->hasFocus() || control->isAncestorOf(QApplication::focusWidget())) return false;
    // Hand the turn to the nearest scrolling area around the control, as if the control were not there.
    for (auto *w = control->parentWidget(); w; w = w->parentWidget()) {
        if (auto *area = qobject_cast<QAbstractScrollArea *>(w)) {
            auto *wheel = static_cast<QWheelEvent *>(event);
            QWheelEvent forwarded(area->viewport()->mapFromGlobal(wheel->globalPosition()), wheel->globalPosition(),
                                  wheel->pixelDelta(), wheel->angleDelta(), wheel->buttons(), wheel->modifiers(),
                                  wheel->phase(), wheel->inverted(), wheel->source());
            QCoreApplication::sendEvent(area->viewport(), &forwarded);
            return true;
        }
        if (w->isWindow()) break;
    }
    return true;  // nothing to scroll: the turn does nothing rather than change the value
}
