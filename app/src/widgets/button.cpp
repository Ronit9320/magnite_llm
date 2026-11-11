#include "button.h"
#include "motion.h"
#include "theme.h"
#include <QEnterEvent>
#include <QPainter>
#include <QStyleOptionButton>
#include <QHelpEvent>
#include <QToolTip>
#include <QIcon>

namespace {
QColor mix(const QColor &a,const QColor &b,qreal t) {
    return QColor::fromRgbF(a.redF()+(b.redF()-a.redF())*t,a.greenF()+(b.greenF()-a.greenF())*t,
                           a.blueF()+(b.blueF()-a.blueF())*t,a.alphaF()+(b.alphaF()-a.alphaF())*t);
}
}
Button::Button(QWidget *parent) : Button(QString(),parent) {}
Button::Button(const QString &text,QWidget *parent) : QPushButton(text,parent) {
    setCursor(Qt::PointingHandCursor);
    auto policy=sizePolicy(); policy.setHeightForWidth(true); setSizePolicy(policy);
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
QRect Button::labelRect() const {
    QStyleOptionButton option; initStyleOption(&option);
    const auto contents=style()->subElementRect(QStyle::SE_PushButtonContents,&option,this);
    const int inset=isFlat() && text().isEmpty()?2:6;
    return contents.intersected(rect().adjusted(inset,2,-inset,-2));
}
int Button::heightForWidth(int width) const {
    if (!text().contains('\n')) return sizeHint().height();
    const auto content=labelRect();
    const int chrome=qMax(12,this->width()-content.width());
    const int textWidth=qMax(1,width-chrome-(icon().isNull()?0:iconSize().width()+6));
    const int height=fontMetrics().boundingRect(QRect(0,0,textWidth,16777215),Qt::TextWordWrap|Qt::TextShowMnemonic,text()).height();
    return qMax(sizeHint().height(),height+qMax(8,this->height()-content.height()));
}
bool Button::event(QEvent *event) {
    if (event->type()==QEvent::ToolTip && m_truncated && toolTip().isEmpty()) {
        auto *help=static_cast<QHelpEvent *>(event);
        QToolTip::showText(help->globalPos(),"<qt>"+text().toHtmlEscaped().replace('\n',"<br/>")+"</qt>",this);
        return true;
    }
    return QPushButton::event(event);
}
void Button::paintEvent(QPaintEvent *) {
    QStyleOptionButton option; initStyleOption(&option);
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    const auto name=objectName();
    const bool primary=name=="primaryBtn" || name=="cardPrimary" || name=="editSave";
    const bool danger=name=="destructiveBtn" || name=="cardDanger" || name=="cancelDownload" || name=="deleteAllChats";
    const bool ghost=isFlat() || name=="historyNew";
    QColor base(primary?Theme::accentPrimary():Theme::surfaceFill());
    QColor hover(primary?Theme::accentHover():danger?Theme::dangerHover():Theme::surfaceFillHover());
    QColor border(primary?Theme::accentPrimary():Theme::surfaceBorder());
    QColor ink(primary?Theme::accentText():danger?Theme::statusError():Theme::textPrimary());
    if (ghost) { base.setAlpha(0); border.setAlpha(0); }
    QColor fill=mix(base,hover,m_hover->value());
    if (isDown()) fill=QColor(primary?Theme::accentActive():danger?Theme::dangerHover():Theme::surfaceFillActive());
    if (isCheckable() && isChecked()) { fill=QColor(primary?Theme::accentActive():Theme::surfaceFillActive()); }
    if (!isEnabled()) { fill=QColor(Theme::insetBackground()); ink=QColor(Theme::textMuted()); border=QColor(Theme::surfaceBorder()); if (ghost) { fill.setAlpha(0); border.setAlpha(0); } }
    if (hasFocus() && isEnabled() && !isDown() && !isChecked()) fill=hover;
    const QRectF bounds=QRectF(rect()).adjusted(1,1,-1,-1);
    painter.setBrush(fill); painter.setPen(QPen(border,1));
    painter.drawRoundedRect(bounds,Theme::cornerRadiusSmall(),Theme::cornerRadiusSmall());
    // Apply the native stylesheet content box before laying out the label.
    // Calling CE_PushButtonLabel with the full widget rect bypasses QSS padding.
    const QRect content=labelRect();
    painter.save(); painter.setClipRect(content);
    if (isDown()) painter.translate(0,0.5);
    painter.setFont(font()); painter.setPen(ink);
    const bool rtl=layoutDirection()==Qt::RightToLeft;
    const bool multiline=text().contains('\n');
    const bool leftAligned=name=="promptSuggestion" || name=="historyNew" || property("leftAligned").toBool();
    const int gap=icon().isNull() || text().isEmpty()?0:6;
    const int iconWidth=icon().isNull()?0:qMin(option.iconSize.width(),content.width());
    const int available=qMax(0,content.width()-iconWidth-gap);
    const QString display=multiline?text():fontMetrics().elidedText(text(),Qt::ElideRight,available,Qt::TextShowMnemonic);
    m_truncated=multiline ? fontMetrics().boundingRect(QRect(0,0,available,16777215),Qt::TextWordWrap|Qt::TextShowMnemonic,display).height()>content.height() : display!=text();
    const int textWidth=qMin(available,fontMetrics().size(Qt::TextShowMnemonic,display).width());
    const int groupWidth=iconWidth+gap+textWidth;
    int start=leftAligned?(rtl?content.right()-groupWidth+1:content.left()):content.left()+(content.width()-groupWidth)/2;
    QRect textRect=content;
    if (!icon().isNull()) {
        const int ix=rtl?start+textWidth+gap:start;
        icon().paint(&painter,QRect(ix,content.center().y()-option.iconSize.height()/2,iconWidth,option.iconSize.height()),Qt::AlignCenter,
                     isEnabled()?QIcon::Normal:QIcon::Disabled,isChecked()?QIcon::On:QIcon::Off);
        textRect=QRect(rtl?start:ix+iconWidth+gap,content.top(),textWidth,content.height());
    }
    const int mnemonic=style()->styleHint(QStyle::SH_UnderlineShortcut,nullptr,this)?Qt::TextShowMnemonic:Qt::TextHideMnemonic;
    const int align=leftAligned?(rtl?Qt::AlignRight:Qt::AlignLeft):Qt::AlignHCenter;
    painter.drawText(textRect,Qt::AlignVCenter|align|mnemonic|(multiline?Qt::TextWordWrap:Qt::TextSingleLine),display);
    painter.restore();
}
