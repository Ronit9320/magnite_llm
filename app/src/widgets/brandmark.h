#pragma once
#include <QColor>
#include <QIcon>
#include <QRectF>
#include <QWidget>
class QPainter;

namespace BrandMark {
// Brand symbol shared by the app icon and activity marks.
void paint(QPainter &painter, const QRectF &bounds, const QColor &color, qreal rotation = 0);
QIcon icon();
}

class BrandSymbol : public QWidget {
public:
    explicit BrandSymbol(QWidget *parent = nullptr);
protected:
    void paintEvent(QPaintEvent *) override;
};

class BrandLockup : public QWidget {
public:
    explicit BrandLockup(int width, QWidget *parent = nullptr);
protected:
    void paintEvent(QPaintEvent *) override;
};
