#pragma once

#include <QObject>

class QScrollBar;
class QWheelEvent;

// Turns notched mouse-wheel steps into short eased glides for every vertical scroll area in the
// app. Each notch moves the same distance Qt would; rapid notches extend the glide instead of
// restarting it. Touchpads and other high-resolution devices already scroll per pixel and are
// left alone, as are modified wheels (zoom, horizontal), combo boxes and spin boxes, and the
// Reduce motion preference. At a scroll limit the event propagates so an outer area can scroll.
class SmoothWheel : public QObject {
    Q_OBJECT
public:
    static void install();
    // Glides `bar` by `pixels` (negative = up). Returns false when the bar cannot move that way.
    static bool glide(QScrollBar *bar, int pixels);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    using QObject::QObject;
    bool handle(QObject *watched, QWheelEvent *wheel);
};
