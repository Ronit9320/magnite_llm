#pragma once

#include "panels/blueprintpreview.h"
#include "widgets/blueprintview.h"

#include <QApplication>
#include <QDialog>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QJsonObject>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QTimer>
#include <functional>

// Shared helpers for the Blueprint check suites (blueprint_tests.cpp, blueprint_editor_tests.cpp).
namespace BlueprintTest {

inline void pump(int ms = 15) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

template<class Widget>
inline Widget *visibleNamed(QWidget &parent, const QString &name) {
    for (auto *widget : parent.findChildren<Widget *>())
        if (widget->isVisible() && widget->accessibleName() == name) return widget;
    return nullptr;
}

inline QPushButton *visibleButton(QWidget &parent, const QString &text) {
    for (auto *button : parent.findChildren<QPushButton *>())
        if (button->isVisible() && (button->text() == text || (button->text().isEmpty() && button->accessibleName() == text)))
            return button;
    return nullptr;
}

inline bool visibleText(QWidget &parent, const QString &fragment) {
    for (const auto *label : parent.findChildren<QLabel *>())
        if (label->isVisible() && label->text().contains(fragment)) return true;
    return false;
}

inline void drag(QWidget *widget, const QPointF &from, const QPointF &to, int holdMs = 0) {
    QMouseEvent press(QEvent::MouseButtonPress, from, widget->mapToGlobal(from.toPoint()),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, to, widget->mapToGlobal(to.toPoint()),
        Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, to, widget->mapToGlobal(to.toPoint()),
        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(widget, &press);
    QCoreApplication::sendEvent(widget, &move);
    if (holdMs) pump(holdMs);
    QCoreApplication::sendEvent(widget, &release);
}

inline void confirmDelete(QWidget *host) {
    QTimer::singleShot(0, host, [] {
        if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
            if (auto *submit = visibleButton(*dialog, "Delete")) submit->click();
            else dialog->reject();
        }
    });
}

inline bool waitForState(BlueprintPreview &preview, const QString &id, const QString &status, int timeout = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout) {
        if (preview.run(id)["status"].toString() == status) {
            pump();  // Let widgets added by runChanged finish entering the layout.
            return true;
        }
        pump(35);
    }
    return false;
}

inline bool waitFor(const std::function<bool()> &condition, int timeout = 6000) {
    QElapsedTimer timer;
    timer.start();
    while (!condition() && timer.elapsed() < timeout) pump(30);
    return condition();
}

inline QJsonObject edge(const QString &from, const QString &out, const QString &to, const QString &in) {
    return {{"from", from}, {"out", out}, {"to", to}, {"in", in}};
}

inline void click(QWidget *w, QPointF at, Qt::KeyboardModifiers modifiers = {}) {
    const QPoint global = w->mapToGlobal(at.toPoint());
    QMouseEvent press(QEvent::MouseButtonPress, at, global, Qt::LeftButton, Qt::LeftButton, modifiers);
    QMouseEvent release(QEvent::MouseButtonRelease, at, global, Qt::LeftButton, Qt::NoButton, modifiers);
    QCoreApplication::sendEvent(w, &press);
    QCoreApplication::sendEvent(w, &release);
}

inline void key(QWidget *w, int key, Qt::KeyboardModifiers modifiers = {}, const QString &text = {}) {
    QKeyEvent press(QEvent::KeyPress, key, modifiers, text);
    QKeyEvent release(QEvent::KeyRelease, key, modifiers, text);
    QCoreApplication::sendEvent(w, &press);
    QCoreApplication::sendEvent(w, &release);
}

inline QPointF viewPoint(BlueprintView &view, QPointF scenePoint) {
    return QPointF(view.mapFromScene(scenePoint));
}

}  // namespace BlueprintTest
