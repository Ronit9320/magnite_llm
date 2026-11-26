#pragma once

#include <QColor>
#include <QEvent>
#include <QFrame>
#include <QImage>
#include <QPalette>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QTimer>
#include <QWidget>

// Finds frames the platform style still draws. Stylesheet borders use theme tokens and ignore the
// widget palette; the style's own frames are painted from palette roles. So a frame whose border
// ring changes when its palette is poisoned is one the theme does not control.
namespace FrameAudit {

inline QString describe(const QWidget *widget) {
    QStringList chain;
    for (const QWidget *at = widget; at && chain.size() < 4; at = at->parentWidget())
        chain.prepend(QString::fromLatin1(at->metaObject()->className()) +
                      (at->objectName().isEmpty() ? QString() : "#" + at->objectName()));
    QString name = chain.join(" > ");
    if (!widget->accessibleName().isEmpty()) name += " (\"" + widget->accessibleName() + "\")";
    return name;
}

// True when any pixel within `ring` pixels of the edge differs between the two images.
inline bool ringDiffers(const QImage &a, const QImage &b, int ring) {
    if (a.size() != b.size()) return true;
    for (int y = 0; y < a.height(); ++y) {
        const bool edgeRow = y < ring || y >= a.height() - ring;
        for (int x = 0; x < a.width(); ++x) {
            if (!edgeRow && x == ring) x = qMax(ring, a.width() - ring);
            if (x < a.width() && a.pixel(x, y) != b.pixel(x, y)) return true;
        }
    }
    return false;
}

// The widget's own painting over a transparent canvas. grab() would first fill the rect with the
// palette's window colour, which would show through transparent borders and count as a frame.
inline QImage paintOf(QWidget *widget) {
    QImage image(widget->size() * widget->devicePixelRatioF(), QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(widget->devicePixelRatioF());
    image.fill(Qt::transparent);
    widget->render(&image, QPoint(), QRegion(), QWidget::DrawChildren);
    return image;
}

inline QStringList nativeFrames(QWidget *root) {
    QStringList found;
    QList<QFrame *> frames = root->findChildren<QFrame *>();
    if (auto *self = qobject_cast<QFrame *>(root)) frames.prepend(self);
    QPalette poison;
    for (int group = 0; group < QPalette::NColorGroups; ++group)
        for (int role = 0; role < QPalette::NColorRoles; ++role)
            poison.setColor(QPalette::ColorGroup(group), QPalette::ColorRole(role), QColor(255, 0, 255));
    for (auto *frame : frames) {
        if (frame->frameShape() == QFrame::NoFrame || frame->frameWidth() <= 0) continue;
        if ((frame != root && !frame->isVisibleTo(root)) || frame->width() < 8 || frame->height() < 8) continue;
        const int ring = frame->frameWidth();
        const QImage before = paintOf(frame);
        const bool ownPalette = frame->testAttribute(Qt::WA_SetPalette);
        const QPalette saved = frame->palette();
        // Children (viewport, scroll bars, headers) keep their colours: only the frame itself is tested.
        struct Shield { QPointer<QWidget> widget; bool own; };
        QList<Shield> shields;
        for (auto *child : frame->findChildren<QWidget *>(Qt::FindDirectChildrenOnly)) {
            shields.append({child, child->testAttribute(Qt::WA_SetPalette)});
            child->setPalette(child->palette());
        }
        frame->setPalette(poison);
        const QImage poisoned = paintOf(frame);
        frame->setPalette(ownPalette ? saved : QPalette());
        for (const auto &shield : shields)
            if (shield.widget && !shield.own) shield.widget->setPalette(QPalette());
        const QImage after = paintOf(frame);
        // Ignore anything animating between grabs; only a stable, palette-driven ring counts.
        if (!ringDiffers(before, after, ring) && ringDiffers(before, poisoned, ring)) found << describe(frame);
    }
    return found;
}

// Audits every window shown while it is installed, once its layout has settled.
class Watcher : public QObject {
public:
    bool eventFilter(QObject *object, QEvent *event) override {
        if (event->type() == QEvent::Show) {
            auto *widget = qobject_cast<QWidget *>(object);
            if (widget && widget->isWindow()) {
                QPointer<QWidget> window(widget);
                QTimer::singleShot(0, this, [this, window] {
                    if (window && window->isVisible())
                        for (const auto &finding : nativeFrames(window)) findings.insert(finding);
                });
            }
        }
        return false;
    }
    QSet<QString> findings;
};

}  // namespace FrameAudit
