#pragma once

#include <QPainterPath>
#include <QVector>

class QPainter;
class QRectF;

// Restored Bezier cels, rendered directly at the display's native resolution.
class LogoAnimation {
public:
    static int frameCount();
    static int framesPerSecond();
    bool paint(QPainter &painter, const QRectF &bounds, qint64 elapsedMs);
private:
    struct Shape { QPainterPath path; float strength; };
    QVector<Shape> m_shapes;
    int m_frame = -1;
};
