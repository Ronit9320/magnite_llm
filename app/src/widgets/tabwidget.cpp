#include "tabwidget.h"
#include "motion.h"
#include "theme.h"
#include <QPainter>
#include <QStyleOptionTab>
#include <QIcon>

SegmentedTabBar::SegmentedTabBar(QWidget *parent) : QTabBar(parent) {
    m_motion=new MotionValue(this,[this](qreal value) {
        m_indicator=QRectF(m_from.topLeft()+(m_to.topLeft()-m_from.topLeft())*value,
                          m_from.size()+(m_to.size()-m_from.size())*value); update();
    });
    setDrawBase(false); setExpanding(false); setElideMode(Qt::ElideRight); setUsesScrollButtons(true);
    connect(this,&QTabBar::currentChanged,this,[this] { selectIndicator(isVisible()); });
}
QSize SegmentedTabBar::tabSizeHint(int index) const {
    const int icon=tabIcon(index).isNull()?0:iconSize().width()+6;
    return {qMax(76,fontMetrics().size(Qt::TextShowMnemonic,tabText(index)).width()+32+icon),qMax(36,fontMetrics().height()+12)};
}
bool SegmentedTabBar::isAnimating() const { return m_motion->running(); }
void SegmentedTabBar::selectIndicator(bool animate) {
    if (currentIndex()<0 || !tabRect(currentIndex()).isValid()) return;
    m_from=m_indicator.isValid()?m_indicator:QRectF(tabRect(currentIndex()).adjusted(3,3,-3,-3));
    m_to=tabRect(currentIndex()).adjusted(3,3,-3,-3);
    m_motion->snap(0); m_motion->to(1,animate,Theme::motionStandardMs());
}
void SegmentedTabBar::resizeEvent(QResizeEvent *event) { QTabBar::resizeEvent(event); selectIndicator(false); }
void SegmentedTabBar::tabLayoutChange() { QTabBar::tabLayoutChange(); if (m_motion) selectIndicator(false); }
void SegmentedTabBar::hideEvent(QHideEvent *event) { m_motion->snap(1); QTabBar::hideEvent(event); }
void SegmentedTabBar::paintEvent(QPaintEvent *) {
    QPainter painter(this); painter.setRenderHint(QPainter::Antialiasing);
    painter.setBrush(QColor(Theme::insetBackground())); painter.setPen(QPen(QColor(Theme::surfaceBorder()),1));
    painter.drawRoundedRect(QRectF(rect()).adjusted(0.5,0.5,-0.5,-0.5),10,10);
    if (m_indicator.isValid()) {
        painter.setBrush(QColor(Theme::surfaceFill())); painter.setPen(QPen(QColor(Theme::surfaceBorder()),1));
        painter.drawRoundedRect(m_indicator,7,7);
        if (hasFocus()) { painter.setBrush(Qt::NoBrush); painter.setPen(QPen(QColor(Theme::accentLabel()),1.5)); painter.drawRoundedRect(m_indicator,7,7); }
    }
    for (int index=0;index<count();++index) {
        const QRect area=tabRect(index).adjusted(12,3,-12,-3);
        const QColor ink(!isTabEnabled(index)?Theme::textMuted():index==currentIndex()?Theme::textPrimary():Theme::textSecondary());
        painter.save(); painter.setClipRect(area); painter.setPen(ink); painter.setFont(font());
        QRect label=area;
        if (!tabIcon(index).isNull()) {
            const bool rtl=layoutDirection()==Qt::RightToLeft;
            const QRect iconRect(rtl?area.right()-iconSize().width()+1:area.left(),area.center().y()-iconSize().height()/2,iconSize().width(),iconSize().height());
            tabIcon(index).paint(&painter,iconRect,Qt::AlignCenter,isTabEnabled(index)?QIcon::Normal:QIcon::Disabled);
            if (rtl) label.setRight(iconRect.left()-6); else label.setLeft(iconRect.right()+7);
        }
        const int mnemonic=style()->styleHint(QStyle::SH_UnderlineShortcut,nullptr,this)?Qt::TextShowMnemonic:Qt::TextHideMnemonic;
        const auto text=fontMetrics().elidedText(tabText(index),Qt::ElideRight,label.width(),Qt::TextShowMnemonic);
        painter.drawText(label,Qt::AlignCenter|Qt::TextSingleLine|mnemonic,text);
        painter.restore();
    }
}
TabWidget::TabWidget(QWidget *parent) : QTabWidget(parent) { setTabBar(new SegmentedTabBar(this)); }
