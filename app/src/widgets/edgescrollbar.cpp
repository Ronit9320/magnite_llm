#include "widgets/edgescrollbar.h"
#include "widgets/smoothscrollbar.h"

#include <QAbstractScrollArea>
#include <QApplication>
#include <QComboBox>
#include <QEvent>
#include <QScrollBar>

EdgeScrollBar *EdgeScrollBar::of(const QAbstractScrollArea *area) {
    return area ? area->findChild<EdgeScrollBar *>(QString(), Qt::FindDirectChildrenOnly) : nullptr;
}

EdgeScrollBar *EdgeScrollBar::attach(QAbstractScrollArea *area) {
    if (!area) return nullptr;
    if (auto *existing = of(area)) return existing;
    return new EdgeScrollBar(area, false);
}

EdgeScrollBar *EdgeScrollBar::attachInside(QAbstractScrollArea *area) {
    if (!area) return nullptr;
    if (auto *existing = of(area)) return existing;
    return new EdgeScrollBar(area, true);
}

EdgeScrollBar::EdgeScrollBar(QAbstractScrollArea *area, bool inside) : QObject(area), m_area(area), m_inside(inside) {
    area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_bar = new SmoothScrollBar(Qt::Vertical, area);   // re-parented to the window once the area is in one
    m_bar->setObjectName("edgeScrollBar");
    m_bar->setCursor(Qt::ArrowCursor);
    m_bar->hide();

    QScrollBar *own = area->verticalScrollBar();
    connect(own, &QScrollBar::rangeChanged, this, [this]() { syncRange(); place(); });
    connect(own, &QScrollBar::valueChanged, m_bar, &QScrollBar::setValue);
    connect(m_bar, &QScrollBar::valueChanged, own, &QScrollBar::setValue);
    syncRange();
    rewire();
    place();
}

EdgeScrollBar::~EdgeScrollBar() {
    if (m_bar) delete m_bar;
}

void EdgeScrollBar::rewire() {
    for (const auto &object : std::as_const(m_watched))
        if (object) object->removeEventFilter(this);
    m_watched.clear();
    for (QWidget *widget = m_area; widget; widget = widget->parentWidget()) {
        widget->installEventFilter(this);
        m_watched.append(widget);
    }
    m_area->viewport()->installEventFilter(this);
    m_watched.append(m_area->viewport());
}

void EdgeScrollBar::syncRange() {
    if (!m_bar) return;
    QScrollBar *own = m_area->verticalScrollBar();
    m_bar->setRange(own->minimum(), own->maximum());
    m_bar->setPageStep(own->pageStep());
    m_bar->setSingleStep(own->singleStep());
    m_bar->setValue(own->value());
}

void EdgeScrollBar::place() {
    if (!m_bar) return;
    QWidget *window = m_inside ? static_cast<QWidget *>(m_area) : m_area->window();
    const bool show = window && (m_inside || window != m_area) && m_area->isVisible() && m_bar->maximum() > m_bar->minimum();
    if (!show) {
        m_bar->hide();
        return;
    }
    if (m_inside) {
        if (m_bar->parentWidget() != window) m_bar->setParent(window);
        m_bar->setGeometry(m_area->width() - kWidth, 0, kWidth, m_area->height());
        m_bar->show();
        m_bar->raise();
        return;
    }
    if (m_bar->parentWidget() != window) m_bar->setParent(window);
    const QPoint top = m_area->mapTo(window, QPoint(0, 0));
    m_bar->setGeometry(window->width() - kWidth, top.y(), kWidth, m_area->height());
    m_bar->show();
    m_bar->raise();
}

bool EdgeScrollBar::eventFilter(QObject *watched, QEvent *event) {
    switch (event->type()) {
    case QEvent::ParentChange:
        rewire();
        place();
        break;
    case QEvent::Resize:
        if (watched == m_area->viewport()) syncRange();
        place();
        break;
    case QEvent::Move:
    case QEvent::Show:
    case QEvent::Hide:
    case QEvent::LayoutRequest:
        place();
        break;
    default:
        break;
    }
    return QObject::eventFilter(watched, event);
}

namespace {
class ScrollAreaAdopter : public QObject {
public:
    using QObject::QObject;
    bool eventFilter(QObject *watched, QEvent *event) override {
        if (event->type() != QEvent::Polish) return false;
        auto *area = qobject_cast<QAbstractScrollArea *>(watched);
        if (!area || EdgeScrollBar::of(area)) return false;
        if (area->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff) return false;       // no vertical bar by design
        if (area->property("transientScroll").toBool() || area->verticalScrollBar()->property("innerScroll").toBool()) return false;
        // Dropdown popups keep their own scrolling. A combo's list is polished briefly before Qt moves it into its popup,
        // so look for the combo box (or a popup window) anywhere above it, not only at its final parent.
        for (QWidget *above = area->parentWidget(); above; above = above->parentWidget())
            if (qobject_cast<QComboBox *>(above) || (above->windowFlags() & Qt::Popup) == Qt::Popup) return false;
        if ((area->windowFlags() & Qt::Popup) == Qt::Popup) return false;
        EdgeScrollBar::attachInside(area);
        return false;
    }
};
}

void EdgeScrollBar::adoptAllScrollAreas() {
    static ScrollAreaAdopter *adopter = nullptr;
    if (adopter || !qApp) return;
    adopter = new ScrollAreaAdopter(qApp);
    qApp->installEventFilter(adopter);
}
