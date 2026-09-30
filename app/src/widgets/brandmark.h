#pragma once
#include <QColor>
#include <QIcon>
#include <QRectF>
class QPainter;

namespace BrandMark {
// Original vector interpretation of the Konark chariot wheel. Fine ornament
// is omitted at icon sizes; the eight principal spokes remain recognizable.
void paint(QPainter &painter, const QRectF &bounds, const QColor &color, qreal rotation = 0);
QIcon icon();
}
