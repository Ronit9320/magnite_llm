#include "widgets/smoothwheel.h"
#include "theme.h"
#include "widgets/motion.h"

#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QComboBox>
#include <QScrollBar>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <cmath>

namespace {
// One running glide per scroll bar, owned by the bar.
class Glide : public QVariantAnimation {
public:
    explicit Glide(QScrollBar *bar) : QVariantAnimation(bar), m_bar(bar) {
        setObjectName("smoothWheelGlide");
        setEasingCurve(QEasingCurve::OutCubic);
        connect(this, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) {
            m_expected = qRound(value.toReal());
            m_bar->setValue(m_expected);
        });
        // Anything else moving the bar (dragging it, keys, code) ends the glide.
        connect(bar, &QScrollBar::valueChanged, this, [this](int value) {
            if (state() == Running && value != m_expected) stop();
        });
        connect(bar, &QScrollBar::sliderPressed, this, &QVariantAnimation::stop);
    }
    static Glide *of(QScrollBar *bar) {
        auto *existing = bar->findChild<QVariantAnimation *>("smoothWheelGlide", Qt::FindDirectChildrenOnly);
        return existing ? static_cast<Glide *>(existing) : new Glide(bar);
    }
    int target() const { return state() == Running ? m_target : m_bar->value(); }
    void to(int target) {
        const int from = m_bar->value();
        stop();
        m_target = target;
        m_expected = from;
        // Longer trips get a little more time so speed stays even.
        const int distance = std::abs(target - from);
        setDuration(qBound(Theme::motionStandardMs(), Theme::motionScrollMs() + distance / 8, 420));
        setStartValue(qreal(from));
        setEndValue(qreal(target));
        start();
    }

private:
    QScrollBar *m_bar;
    int m_target = 0;
    int m_expected = 0;
};

QAbstractScrollArea *areaForViewport(QObject *watched) {
    auto *widget = qobject_cast<QWidget *>(watched);
    if (!widget) return nullptr;
    auto *area = qobject_cast<QAbstractScrollArea *>(widget->parentWidget());
    return area && area->viewport() == widget ? area : nullptr;
}
}

void SmoothWheel::install() {
    static SmoothWheel *filter = nullptr;
    if (filter || !qApp) return;
    filter = new SmoothWheel(qApp);
    qApp->installEventFilter(filter);
}

bool SmoothWheel::glide(QScrollBar *bar, int pixels) {
    if (!bar || pixels == 0 || !bar->isEnabled() || bar->maximum() <= bar->minimum()) return false;
    auto *glide = Glide::of(bar);
    const int start = glide->target();
    const int target = qBound(bar->minimum(), start + pixels, bar->maximum());
    if (target == start) return glide->state() == QAbstractAnimation::Running;   // keep a glide into the edge
    glide->to(target);
    return true;
}

bool SmoothWheel::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() == QEvent::Wheel && handle(watched, static_cast<QWheelEvent *>(event))) {
        event->accept();
        return true;
    }
    return QObject::eventFilter(watched, event);
}

bool SmoothWheel::handle(QObject *watched, QWheelEvent *wheel) {
    // High-resolution devices (touchpads, Magic Mouse) already move per pixel with momentum.
    if (!wheel->pixelDelta().isNull() || wheel->phase() != Qt::NoScrollPhase) return false;
    if (wheel->modifiers() != Qt::NoModifier) return false;   // zoom, horizontal and app shortcuts
    const QPoint delta = wheel->angleDelta();
    if (delta.y() == 0 || std::abs(delta.x()) > std::abs(delta.y())) return false;
    if (MotionValue::reduced()) return false;

    QScrollBar *bar = nullptr;
    int step = 0;
    if (auto *area = areaForViewport(watched)) {
        if (area->property("transientScroll").toBool()) return false;   // has its own wheel rules
        if (qobject_cast<QComboBox *>(area->parentWidget())) return false;
        if ((area->window()->windowFlags() & Qt::Popup) == Qt::Popup) return false;
        if (auto *view = qobject_cast<QAbstractItemView *>(area);
            view && view->verticalScrollMode() == QAbstractItemView::ScrollPerItem)
            return false;   // item units, not pixels
        bar = area->verticalScrollBar();
        if (area->verticalScrollBarPolicy() == Qt::ScrollBarAlwaysOff && bar->maximum() <= bar->minimum())
            return false;
    } else if (auto *edge = qobject_cast<QScrollBar *>(watched);
               edge && edge->orientation() == Qt::Vertical && edge->objectName() == "edgeScrollBar") {
        bar = edge;   // the floating bar mirrors its area's own bar
    } else {
        return false;
    }
    // A notched wheel reports 120 per notch; move as far as Qt's own step would.
    step = QApplication::wheelScrollLines() * bar->singleStep();
    return glide(bar, -qRound(delta.y() / 120.0 * step));
}
