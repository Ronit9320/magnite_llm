#include "widgets/brandmark.h"
#include "theme.h"
#include <QPainter>
#include <QFile>
#include <QSvgRenderer>
#include <QPixmap>

namespace {
QByteArray artwork(const QString &name) {
    QFile file(":/brand/" + name + ".svg");
    if (!file.open(QIODevice::ReadOnly)) return {};
    auto svg = file.readAll();
    svg.replace("@brandOrange", Theme::brandOrange().toUtf8());
    svg.replace("@brandInk", Theme::brandInk().toUtf8());
    svg.replace("@brandText", Theme::brandText().toUtf8());
    return svg;
}
}

void BrandMark::paint(QPainter &p, const QRectF &bounds, const QColor &, qreal rotation) {
    static QSvgRenderer symbol(artwork("symbol"));
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    // Inset keeps the rotating silhouette inside the activity ring.
    const qreal size = qMin(bounds.width(), bounds.height()) * 0.9;
    p.translate(bounds.center());
    p.rotate(rotation);
    symbol.render(&p, QRectF(-size/2, -size/2, size, size));
    p.restore();
}

BrandWordmark::BrandWordmark(QWidget *parent) : QWidget(parent) {
    setFixedSize(132, 37);
    setAccessibleName("Magnite");
}

void BrandWordmark::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    QSvgRenderer wordmark(artwork("wordmark"));
    wordmark.render(&painter, QRectF(rect()));
}

QIcon BrandMark::icon() {
    QIcon result;
    for (int size : {16,24,32,48,64,128,256}) {
        QPixmap pixmap(size,size); pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        paint(painter,QRectF(0,0,size,size),QColor(Theme::accentPrimary()));
        painter.end(); result.addPixmap(pixmap);
    }
    return result;
}
