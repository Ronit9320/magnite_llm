#pragma once
#include <QColor>
#include <QIcon>
#include <QRectF>
#include <QWidget>
class QPainter;

namespace BrandMark {
// Supplied Magnite artwork, shared by the app icon and animated activity marks.
void paint(QPainter &painter, const QRectF &bounds, const QColor &color, qreal rotation = 0);
QIcon icon();
}

class BrandWordmark : public QWidget {
public:
    explicit BrandWordmark(QWidget *parent = nullptr);
protected:
    void paintEvent(QPaintEvent *) override;
};
