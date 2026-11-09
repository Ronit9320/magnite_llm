#include "progressbar.h"
#include "motion.h"
#include "theme.h"
#include <QPainter>
#include <QPainterPath>
#include <QStyleOptionProgressBar>

ProgressBar::ProgressBar(QWidget *parent) : QProgressBar(parent) {
    m_motion=new MotionValue(this,[this](qreal) { update(); });
    connect(this,&QProgressBar::valueChanged,this,[this](int value) {
        const bool animate=isVisible() && value>m_motion->value() && value>minimum();
        m_motion->to(value,animate,Theme::motionStandardMs());
    });
}
bool ProgressBar::isAnimating() const { return m_motion->running(); }
void ProgressBar::hideEvent(QHideEvent *event) { m_motion->snap(qMax(minimum(),value())); QProgressBar::hideEvent(event); }
void ProgressBar::paintEvent(QPaintEvent *event) {
    if ((minimum()==0 && maximum()==0) || orientation()!=Qt::Horizontal) { QProgressBar::paintEvent(event); return; }
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    const QRectF bounds=rect();
    const qreal radius=qMin(4.0,height()/2.0);
    painter.setPen(Qt::NoPen); painter.setBrush(QColor(Theme::insetBackground())); painter.drawRoundedRect(bounds,radius,radius);
    const qreal portion=value()>=minimum() && maximum()>minimum()?qBound(0.0,(m_motion->value()-minimum())/(qreal(maximum())-minimum()),1.0):0;
    if (portion>0) {
        QPainterPath clip; clip.addRoundedRect(bounds,radius,radius); painter.setClipPath(clip);
        const qreal width=bounds.width()*portion;
        const bool reverse=(layoutDirection()==Qt::RightToLeft)!=invertedAppearance();
        painter.setBrush(QColor(isEnabled()?Theme::accentPrimary():Theme::textMuted()));
        painter.drawRoundedRect(QRectF(reverse?bounds.right()-width:bounds.left(),bounds.top(),width,bounds.height()),radius,radius);
        painter.setClipping(false);
    }
    if (isTextVisible()) { QStyleOptionProgressBar option; initStyleOption(&option); style()->drawControl(QStyle::CE_ProgressBarLabel,&option,&painter,this); }
}
