#include "widgets/logoanimation.h"
#include "theme.h"

#include <QDataStream>
#include <QFile>
#include <QPainter>

namespace {
struct Clip {
    quint16 fps = 0;
    QVector<QByteArray> frames;
    Clip() {
        QFile file(":/brand/animated-symbol.mgl");
        if (!file.open(QIODevice::ReadOnly)) return;
        const QByteArray data = qUncompress(file.readAll());
        if (!data.startsWith("MGL1")) return;
        QDataStream stream(data);
        stream.skipRawData(4);
        quint16 count = 0;
        stream >> fps >> count;
        if (fps != 30 || count != 540) { fps = 0; return; }
        for (int i = 0; i < count; ++i) {
            quint32 size = 0;
            stream >> size;
            if (size > quint32(data.size()) || size > quint32(stream.device()->bytesAvailable())) {
                frames.clear(); fps = 0; return;
            }
            frames.append(stream.device()->read(size));
        }
    }
};
const Clip &clip() { static const Clip value; return value; }

QPointF point(QDataStream &stream) {
    float x = 0, y = 0;
    stream >> x >> y;
    return {x, y};
}
}

int LogoAnimation::frameCount() { return clip().frames.size(); }
int LogoAnimation::framesPerSecond() { return clip().fps; }

bool LogoAnimation::paint(QPainter &painter, const QRectF &bounds, qint64 elapsedMs) {
    if (clip().frames.isEmpty()) return false;
    const int index = (qMax(qint64(0), elapsedMs) * clip().fps / 1000) % clip().frames.size();
    if (m_frame != index) {
        QVector<Shape> shapes;
        QDataStream stream(clip().frames[index]);
        stream.setFloatingPointPrecision(QDataStream::SinglePrecision);
        quint16 count = 0;
        stream >> count;
        for (int i = 0; i < count; ++i) {
            Shape shape;
            quint16 contours = 0;
            stream >> shape.strength >> contours;
            shape.path.setFillRule(Qt::WindingFill);
            for (int j = 0; j < contours; ++j) {
                quint16 commands = 0;
                stream >> commands;
                for (int k = 0; k < commands; ++k) {
                    quint8 command = 0;
                    stream >> command;
                    if (command == 'M') shape.path.moveTo(point(stream));
                    else if (command == 'L') shape.path.lineTo(point(stream));
                    else if (command == 'C') {
                        const auto control1 = point(stream);
                        const auto control2 = point(stream);
                        const auto end = point(stream);
                        shape.path.cubicTo(control1, control2, end);
                    } else return false;
                }
                shape.path.closeSubpath();
            }
            shapes.append(std::move(shape));
        }
        if (stream.status() != QDataStream::Ok) return false;
        m_shapes = std::move(shapes);
        m_frame = index;
    }
    const QColor orange(Theme::brandOrange()), ink(Theme::brandInk());
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.translate(bounds.topLeft());
    painter.scale(bounds.width() / 420.0, bounds.height() / 420.0);
    for (const auto &shape : m_shapes) {
        const float t = qBound(0.0f, shape.strength, 1.0f);
        const auto blend = [t](int a, int b) { return qRound(a + (b - a) * t); };
        painter.fillPath(shape.path, QColor(blend(orange.red(), ink.red()),
                                           blend(orange.green(), ink.green()),
                                           blend(orange.blue(), ink.blue())));
    }
    painter.restore();
    return true;
}
