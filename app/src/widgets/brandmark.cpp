#include "widgets/brandmark.h"
#include "theme.h"
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

void BrandMark::paint(QPainter &p, const QRectF &bounds, const QColor &color, qreal rotation) {
    p.save();
    p.setRenderHint(QPainter::Antialiasing);
    const qreal size = qMin(bounds.width(), bounds.height());
    const bool detail = size >= 56;
    p.translate(bounds.center());
    p.scale(size / 216.0, size / 216.0);
    p.rotate(rotation);
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(color, detail ? 3.0 : 5.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.drawEllipse(QPointF(0,0),96,96);
    p.drawEllipse(QPointF(0,0),82,82);
    if (size < 40) {
        // A bold silhouette survives taskbar and small activity-indicator sizes.
        p.setPen(QPen(color,8,Qt::SolidLine,Qt::RoundCap));
        for (int i=0;i<8;++i) { p.drawLine(QPointF(0,-22),QPointF(0,-81)); p.rotate(45); }
        p.setPen(Qt::NoPen); p.setBrush(color); p.drawEllipse(QPointF(0,0),14,14);
        p.restore();
        return;
    }
    // Beaded rim, interspersed with eight diamond ornaments.
    if (detail) {
        p.save();
        p.setPen(Qt::NoPen); p.setBrush(color);
        for (int i=0; i<64; ++i) {
            if (i%8 == 0) {
                QPainterPath diamond;
                diamond.moveTo(0,-94); diamond.lineTo(3.5,-89);
                diamond.lineTo(0,-84); diamond.lineTo(-3.5,-89); diamond.closeSubpath();
                p.drawPath(diamond);
            } else p.drawEllipse(QPointF(0,-89),1.5,1.5);
            p.rotate(5.625);
        }
        p.restore();
    }
    // Eight slender spokes alternate with eight sculpted principal spokes.
    for (int i=0; i<8; ++i) {
        p.save(); p.rotate(22.5);
        p.setPen(QPen(color,detail ? 2.5 : 3.5,Qt::SolidLine,Qt::RoundCap));
        p.drawLine(QPointF(0,-25),QPointF(0,-82));
        if (detail) {
            p.setBrush(color); p.drawEllipse(QPointF(0,-66),2.5,2.5);
        }
        p.restore();
        p.setPen(QPen(color,detail ? 2.5 : 3.5,Qt::SolidLine,Qt::RoundCap,Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        QPainterPath spoke;
        spoke.moveTo(-5,-25); spoke.lineTo(-5,-42);
        spoke.cubicTo(-15,-47,-15,-61,-6,-67);
        spoke.lineTo(-7,-82); spoke.lineTo(7,-82); spoke.lineTo(6,-67);
        spoke.cubicTo(15,-61,15,-47,5,-42);
        spoke.lineTo(5,-25); p.drawPath(spoke);
        if (detail) {
            p.drawEllipse(QPointF(0,-55),6.5,6.5);
            p.setBrush(color); p.drawEllipse(QPointF(0,-55),2,2);
            p.setBrush(Qt::NoBrush);
            p.drawLine(QPointF(0,-70),QPointF(0,-78));
        }
        p.rotate(45);
    }
    p.setBrush(Qt::NoBrush);
    p.setPen(QPen(color,detail ? 2.5 : 4));
    p.drawEllipse(QPointF(0,0),25,25);
    p.drawEllipse(QPointF(0,0),16,16);
    if (detail) {
        p.save(); p.setPen(Qt::NoPen); p.setBrush(color);
        for (int i=0;i<16;++i) { p.drawEllipse(QPointF(0,-20.5),1.2,1.2); p.rotate(22.5); }
        p.restore();
    }
    p.setPen(Qt::NoPen); p.setBrush(color);
    p.drawEllipse(QPointF(0,0),8,8);
    p.restore();
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
