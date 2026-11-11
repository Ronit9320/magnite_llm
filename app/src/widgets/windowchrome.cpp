#include "widgets/windowchrome.h"
#include "widgets/button.h"
#include "theme.h"

#include <QApplication>
#include <QDialog>
#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QMainWindow>
#include <QWindow>
#include <QPointer>
#include <QCursor>

namespace {
constexpr int headerHeight = 36;
constexpr int resizeMargin = 5;

void toggleMaximized(QWidget *window) {
    if (window->isMaximized()) window->showNormal();
    else window->showMaximized();
}

void addControls(QWidget *window, QWidget *header, bool mainWindow) {
    auto *layout = qobject_cast<QHBoxLayout *>(header->layout());
    auto add = [&](const QString &icon, const QString &name, const QString &accessible) {
        auto *button = new Button(header);
        button->setProperty("themeIcon", icon);
        button->setIcon(Theme::coloredIcon(icon, QColor(Theme::textPrimary()), 14));
        button->setIconSize(QSize(14, 14));
        button->setObjectName(name);
        button->setAccessibleName(accessible);
        button->setToolTip(accessible);
        button->setFlat(true);
        button->setFixedSize(30, 28);
        button->setAutoDefault(false);
        layout->addWidget(button);
        return button;
    };
    if (mainWindow) {
        auto *minimize = add("window-minimize", "windowMinimize", "Minimize window");
        QObject::connect(minimize, &QPushButton::clicked, window, &QWidget::showMinimized);
        auto *maximize = add("window-maximize", "windowMaximize", "Maximize window");
        QObject::connect(maximize, &QPushButton::clicked, window, [window] { toggleMaximized(window); });
    }
    auto *close = add("window-close", "windowClose", "Close window");
    QObject::connect(close, &QPushButton::clicked, window, &QWidget::close);
    header->setProperty("windowDragRegion", true);
    window->setProperty("appWindowChrome", true);
}

class ChromeEvents : public QObject {
public:
    explicit ChromeEvents(QObject *parent) : QObject(parent) {}
protected:
    bool eventFilter(QObject *watched, QEvent *event) override {
        auto *widget = qobject_cast<QWidget *>(watched);
        if (!widget) return false;
        if (event->type() == QEvent::Polish && widget->isWindow() && qobject_cast<QDialog *>(widget)) {
            if (widget->property("appWindowChrome").toBool()) return false;
            widget->setProperty("appWindowChrome", true);
            // Settings already supplies its own frame and close controls.
            if (!widget->windowFlags().testFlag(Qt::FramelessWindowHint)) {
                widget->setWindowFlag(Qt::FramelessWindowHint);
                const auto margins = widget->contentsMargins();
                widget->setContentsMargins(margins.left(), margins.top() + headerHeight,
                                          margins.right(), margins.bottom());
                auto *header = new QFrame(widget);
                header->setObjectName("appWindowHeader");
                auto *layout = new QHBoxLayout(header);
                layout->setContentsMargins(12, 0, 6, 0);
                layout->setSpacing(4);
                auto *title = new QLabel(widget->windowTitle(), header);
                title->setObjectName("appWindowTitle");
                title->setMinimumWidth(0);
                title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                title->setAttribute(Qt::WA_TransparentForMouseEvents);
                layout->addWidget(title, 1);
                QObject::connect(widget, &QWidget::windowTitleChanged, title, &QLabel::setText);
                addControls(widget, header, false);
                header->setGeometry(0, 0, widget->width(), headerHeight);
            }
        }
        auto *window = widget->window();
        if (!window->property("appWindowChrome").toBool()) return false;
        if (widget == window && event->type() == QEvent::Resize) {
            if (auto *header = window->findChild<QWidget *>("appWindowHeader", Qt::FindDirectChildrenOnly)) {
                header->setGeometry(0, 0, window->width(), headerHeight);
                header->raise();
            }
        }
        if (widget == window && event->type() == QEvent::WindowStateChange) {
            if (auto *button = window->findChild<Button *>("windowMaximize")) {
                const QString icon = window->isMaximized() ? "window-restore" : "window-maximize";
                button->setProperty("themeIcon", icon);
                button->setIcon(Theme::coloredIcon(icon, QColor(Theme::textPrimary()), 14));
                button->setAccessibleName(window->isMaximized() ? "Restore window" : "Maximize window");
                button->setToolTip(button->accessibleName());
            }
        }
        if (event->type() == QEvent::Show) widget->setMouseTracking(true);
        if (event->type() == QEvent::Leave && m_cursorWidget == widget) restoreCursor();
        if (event->type() != QEvent::MouseButtonPress && event->type() != QEvent::MouseButtonDblClick &&
            event->type() != QEvent::MouseMove) return false;
        auto *mouse = static_cast<QMouseEvent *>(event);
        if (!window->windowHandle()) return false;
        const QPoint pos = window->mapFromGlobal(mouse->globalPosition().toPoint());
        Qt::Edges edges;
        if (!window->isMaximized() && !window->isFullScreen()) {
            if (window->minimumWidth() < window->maximumWidth()) {
                if (pos.x() < resizeMargin) edges |= Qt::LeftEdge;
                if (pos.x() >= window->width() - resizeMargin) edges |= Qt::RightEdge;
            }
            if (window->minimumHeight() < window->maximumHeight()) {
                if (pos.y() < resizeMargin) edges |= Qt::TopEdge;
                if (pos.y() >= window->height() - resizeMargin) edges |= Qt::BottomEdge;
            }
        }
        if (event->type() == QEvent::MouseMove) {
            if (m_cursorWidget != widget || !edges) restoreCursor();
            if (edges) {
                if (!m_cursorWidget) { m_cursorWidget = widget; m_originalCursor = widget->cursor(); }
                const bool horizontal = edges.testFlag(Qt::LeftEdge) || edges.testFlag(Qt::RightEdge);
                const bool vertical = edges.testFlag(Qt::TopEdge) || edges.testFlag(Qt::BottomEdge);
                const bool descending = (edges.testFlag(Qt::LeftEdge) && edges.testFlag(Qt::TopEdge)) ||
                                        (edges.testFlag(Qt::RightEdge) && edges.testFlag(Qt::BottomEdge));
                widget->setCursor(horizontal && vertical ? (descending ? Qt::SizeFDiagCursor : Qt::SizeBDiagCursor) :
                                  horizontal ? Qt::SizeHorCursor : Qt::SizeVerCursor);
            }
            return false;
        }
        if (mouse->button() != Qt::LeftButton) return false;
        if (edges && event->type() == QEvent::MouseButtonPress)
            return window->windowHandle()->startSystemResize(edges);
        bool dragRegion = false;
        for (auto *target = widget; target && target != window; target = target->parentWidget()) {
            // Buttons and editors in a masthead must retain normal interaction.
            if (qobject_cast<QAbstractButton *>(target)) return false;
            if (target->property("windowDragRegion").toBool()) { dragRegion = true; break; }
        }
        if (!dragRegion) return false;
        if (event->type() == QEvent::MouseButtonDblClick && qobject_cast<QMainWindow *>(window)) {
            toggleMaximized(window);
            return true;
        }
        if (event->type() == QEvent::MouseButtonPress) return window->windowHandle()->startSystemMove();
        return false;
    }
private:
    void restoreCursor() {
        if (m_cursorWidget) m_cursorWidget->setCursor(m_originalCursor);
        m_cursorWidget.clear();
    }
    QPointer<QWidget> m_cursorWidget;
    QCursor m_originalCursor;
};
} // namespace

// macOS keeps its native frame (traffic lights, title bar, resize and drag), so
// the app-owned chrome below is installed on Linux and Windows only.
void WindowChrome::install(QApplication &application) {
#ifdef Q_OS_MACOS
    Q_UNUSED(application);
#else
    application.installEventFilter(new ChromeEvents(&application));
#endif
}

void WindowChrome::attachMainWindow(QWidget *window, QWidget *masthead) {
#ifdef Q_OS_MACOS
    Q_UNUSED(window);
    Q_UNUSED(masthead);
    return;
#else
    window->setWindowFlag(Qt::FramelessWindowHint);
    addControls(window, masthead, true);
#endif
}
