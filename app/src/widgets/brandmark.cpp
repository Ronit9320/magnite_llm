#include "widgets/brandmark.h"
#include "theme.h"
#include <QPainter>
#include <QFile>
#include <QSvgRenderer>
#include <QPixmap>

namespace {
constexpr QSizeF lockupSize(691, 106);
constexpr QRectF lockupSymbol(0, 0, 106, 106);
constexpr QRectF lockupWordmark(128.929, 10, 562, 85);

QByteArray artwork(const QString &name, const QString &text = {}) {
    QFile file(":/brand/" + name + ".svg");
    if (!file.open(QIODevice::ReadOnly)) return {};
    auto svg = file.readAll();
    svg.replace("@brandOrange", Theme::brandOrange().toUtf8());
    svg.replace("@brandInk", Theme::brandInk().toUtf8());
    svg.replace("@brandText", text.toUtf8());
    return svg;
}

QSvgRenderer &brandSymbol() {
    static QSvgRenderer symbol(artwork("symbol"));
    return symbol;
}
}

void BrandMark::paint(QPainter &p, const QRectF &bounds, const QColor &, qreal rotation) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    // Inset keeps the rotating silhouette inside the activity ring.
    const qreal size = qMin(bounds.width(), bounds.height()) * 0.9;
    p.translate(bounds.center());
    p.rotate(rotation);
    brandSymbol().render(&p, QRectF(-size/2, -size/2, size, size));
    p.restore();
}

BrandSymbol::BrandSymbol(QWidget *parent) : QWidget(parent) {
    setAccessibleName("Magnite Space logo");
}

void BrandSymbol::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    BrandMark::paint(painter, QRectF(rect()), QColor(Theme::brandOrange()));
}

BrandLockup::BrandLockup(int width, QWidget *parent) : QWidget(parent) {
    setFixedSize(width, qRound(width * lockupSize.height() / lockupSize.width()));
    setAccessibleName("Magnite Space");
}

void BrandLockup::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    const qreal scale = qMin(width() / lockupSize.width(), height() / lockupSize.height());
    painter.translate((width() - scale * lockupSize.width()) / 2,
                      (height() - scale * lockupSize.height()) / 2);
    painter.scale(scale, scale);
    brandSymbol().render(&painter, lockupSymbol);
    QSvgRenderer wordmark(artwork("wordmark", Theme::brandText()));
    wordmark.render(&painter, lockupWordmark);
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
