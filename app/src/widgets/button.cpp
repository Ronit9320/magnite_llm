#include "button.h"
#include "motion.h"
#include "theme.h"
#include <QEnterEvent>
#include <QPainter>
#include <QStyleOptionButton>

namespace {
QColor mix(const QColor &a,const QColor &b,qreal t) {
    return QColor::fromRgbF(a.redF()+(b.redF()-a.redF())*t,a.greenF()+(b.greenF()-a.greenF())*t,
                           a.blueF()+(b.blueF()-a.blueF())*t,a.alphaF()+(b.alphaF()-a.alphaF())*t);
}
}
Button::Button(QWidget *parent) : Button(QString(),parent) {}
Button::Button(const QString &text,QWidget *parent) : QPushButton(text,parent) {
    setCursor(Qt::PointingHandCursor);
    setProperty("animatedComponent",true);
    m_hover=new MotionValue(this,[this](qreal) { update(); });
    connect(this,&QPushButton::pressed,this,qOverload<>(&QWidget::update));
    connect(this,&QPushButton::released,this,qOverload<>(&QWidget::update));
}
bool Button::isAnimating() const { return m_hover->running(); }
void Button::enterEvent(QEnterEvent *event) { QPushButton::enterEvent(event); m_hover->to(isEnabled()?1:0); }
void Button::leaveEvent(QEvent *event) { QPushButton::leaveEvent(event); m_hover->to(0); }
void Button::hideEvent(QHideEvent *event) { m_hover->snap(0); QPushButton::hideEvent(event); }
void Button::changeEvent(QEvent *event) {
    QPushButton::changeEvent(event);
    if (event->type()==QEvent::EnabledChange) m_hover->snap(0);
    update();
}
void Button::paintEvent(QPaintEvent *) {
    QStyleOptionButton option; initStyleOption(&option);
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    const auto name=objectName();
    const bool primary=name=="primaryBtn" || name=="cardPrimary" || name=="editSave";
    const bool danger=name=="destructiveBtn" || name=="cardDanger" || name=="cancelDownload";
    const bool ghost=isFlat() || name=="historyNew";
    QColor base(primary?Theme::accentPrimary():Theme::surfaceFill());
    QColor hover(primary?Theme::accentHover():danger?Theme::dangerHover():Theme::surfaceFillHover());
    QColor border(primary?Theme::accentPrimary():Theme::surfaceBorder());
    QColor ink(primary?Theme::accentText():danger?Theme::statusError():Theme::textPrimary());
    if (ghost) { base.setAlpha(0); border.setAlpha(0); }
    QColor fill=mix(base,hover,m_hover->value());
    if (isDown()) fill=QColor(primary?Theme::accentActive():danger?Theme::dangerHover():Theme::surfaceFillActive());
    if (isCheckable() && isChecked()) { fill=QColor(primary?Theme::accentActive():Theme::surfaceFillActive()); border=QColor(Theme::accentLabel()); }
    if (!isEnabled()) { fill=QColor(Theme::insetBackground()); ink=QColor(Theme::textMuted()); border=QColor(Theme::surfaceBorder()); if (ghost) { fill.setAlpha(0); border.setAlpha(0); } }
    const QRectF bounds=QRectF(rect()).adjusted(1,1,-1,-1);
    painter.setBrush(fill); painter.setPen(QPen(border,1));
    painter.drawRoundedRect(bounds,Theme::cornerRadiusSmall(),Theme::cornerRadiusSmall());
    if (hasFocus() && isEnabled()) {
        QColor ring(Theme::accentLabel()); ring.setAlphaF(0.25);
        painter.setBrush(Qt::NoBrush); painter.setPen(QPen(ring,3));
        painter.drawRoundedRect(bounds.adjusted(1,1,-1,-1),7,7);
        painter.setPen(QPen(QColor(Theme::accentLabel()),1)); painter.drawRoundedRect(bounds,8,8);
    }
    option.palette.setColor(QPalette::ButtonText,ink);
    option.palette.setColor(QPalette::WindowText,ink);
    // Keep native icon, mnemonic, default-button and accessibility semantics.
    if (isDown()) painter.translate(0,0.5);
    style()->drawControl(QStyle::CE_PushButtonLabel,&option,&painter,this);
}
