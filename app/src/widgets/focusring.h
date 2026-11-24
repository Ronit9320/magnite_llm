#pragma once

#include "theme.h"

#include <QAbstractButton>
#include <QAbstractSlider>
#include <QApplication>
#include <QColor>
#include <QEvent>
#include <QFocusEvent>
#include <QPainter>
#include <QPointer>
#include <QTabBar>
#include <QWidget>

// Draws an accent ring around a button, slider or tab bar when it
// receives focus from the keyboard (Tab, Shift+Tab, a shortcut). Mouse clicks
// never show it. Fields and combo boxes use ComponentFocus instead.
// Install once with KeyboardFocusRing::install().
class KeyboardFocusRing : public QObject {
public:
    static void install() {
        if (!instance()) {
            instance() = new KeyboardFocusRing(qApp);
            qApp->installEventFilter(instance());
        }
    }

    // The control the ring currently surrounds (for tests), or nullptr.
    static QWidget *target() { return instance() ? instance()->m_target.data() : nullptr; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        switch (event->type()) {
        case QEvent::FocusIn: {
            auto *widget = qobject_cast<QWidget *>(watched);
            const auto reason = static_cast<QFocusEvent *>(event)->reason();
            const bool keyboard = reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason
                                  || reason == Qt::ShortcutFocusReason;
            if (widget && keyboard && wantsRing(widget)) attach(widget);
            else if (widget != m_overlay) detach();
            break;
        }
        case QEvent::FocusOut:
            if (watched == m_target) detach();
            break;
        case QEvent::Move:
        case QEvent::Resize:
            if (watched == m_target) place();
            break;
        case QEvent::Hide:
            if (watched == m_target) detach();
            break;
        case QEvent::StyleChange:
            if (watched == m_target && m_overlay) m_overlay->update();
            break;
        default:
            break;
        }
        return false;
    }

private:
    static KeyboardFocusRing *&instance() {
        static KeyboardFocusRing *ring = nullptr;
        return ring;
    }

    class Overlay : public QWidget {
    public:
        explicit Overlay(QWidget *parent) : QWidget(parent) {
            setObjectName(QStringLiteral("keyboardFocusRing"));
            setAttribute(Qt::WA_TransparentForMouseEvents);
            setAttribute(Qt::WA_NoSystemBackground);
            setFocusPolicy(Qt::NoFocus);
        }

    protected:
        void paintEvent(QPaintEvent *) override {
            QPainter painter(this);
            painter.setRenderHint(QPainter::Antialiasing);
            QPen pen{QColor(Theme::accentLabel())}; // braces: not a function declaration (MSVC, clang)
            pen.setWidthF(2.0);
            painter.setPen(pen);
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 9, 9);
        }
    };

    using QObject::QObject;

    // Fields and combo boxes already get ComponentFocus's halo; this covers the rest.
    static bool wantsRing(QWidget *widget) {
        return qobject_cast<QAbstractButton *>(widget) || qobject_cast<QAbstractSlider *>(widget)
               || qobject_cast<QTabBar *>(widget);
    }

    void attach(QWidget *widget) {
        if (!widget->parentWidget()) return;
        if (m_target != widget) detach();
        m_target = widget;
        if (!m_overlay || m_overlay->parentWidget() != widget->parentWidget()) {
            delete m_overlay;
            m_overlay = new Overlay(widget->parentWidget());
        }
        place();
        m_overlay->show();
        m_overlay->raise();
    }

    void detach() {
        m_target = nullptr;
        if (m_overlay) m_overlay->hide();
    }

    void place() {
        if (!m_target || !m_overlay) return;
        m_overlay->setGeometry(m_target->geometry().adjusted(-3, -3, 3, 3));
        m_overlay->raise();
    }

    QPointer<QWidget> m_target;
    QPointer<Overlay> m_overlay;
};
