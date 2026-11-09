#include "widgets/combobox.h"
#include "theme.h"
#include "widgets/motion.h"
#include <QApplication>

#include <QIcon>
#include <QListView>
#include <QPainter>
#include <QStyleOptionComboBox>
#include <QStyledItemDelegate>

namespace {
class SelectDelegate : public QStyledItemDelegate {
public:
    SelectDelegate(QComboBox *combo,QObject *parent) : QStyledItemDelegate(parent),m_combo(combo) {}
    QSize sizeHint(const QStyleOptionViewItem &option,const QModelIndex &index) const override {
        auto size=QStyledItemDelegate::sizeHint(option,index); size.setHeight(qMax(36,size.height())); return size;
    }
    void paint(QPainter *p,const QStyleOptionViewItem &option,const QModelIndex &index) const override {
        QStyleOptionViewItem item(option); initStyleOption(&item,index);
        p->save(); p->setRenderHint(QPainter::Antialiasing);
        const auto row=QRectF(option.rect).adjusted(3,1,-3,-1);
        if (option.state.testFlag(QStyle::State_Selected) || option.state.testFlag(QStyle::State_MouseOver)) {
            p->setPen(Qt::NoPen); p->setBrush(QColor(Theme::surfaceFillHover())); p->drawRoundedRect(row,5,5);
        }
        QRect text=option.rect.adjusted(12,0,-32,0);
        const bool rtl=m_combo->layoutDirection()==Qt::RightToLeft;
        if (rtl) text=option.rect.adjusted(32,0,-12,0);
        const bool enabled=index.flags().testFlag(Qt::ItemIsEnabled);
        p->setPen(QColor(enabled?Theme::textPrimary():Theme::textMuted())); p->setFont(option.font);
        p->drawText(text,Qt::AlignVCenter|(rtl?Qt::AlignRight:Qt::AlignLeft),option.fontMetrics.elidedText(item.text,Qt::ElideMiddle,text.width()));
        if (index.row()==m_combo->currentIndex() && m_combo->currentText()==item.text)
            Theme::coloredIcon("check",QColor(enabled?Theme::accentLabel():Theme::textMuted()),14)
                .paint(p,QRect(rtl?option.rect.left()+10:option.rect.right()-24,option.rect.center().y()-7,14,14));
        p->restore();
    }
private:
    QComboBox *m_combo;
};
}

ComboBox::ComboBox(QWidget *parent) : QComboBox(parent) {
    m_arrow=new MotionValue(this,[this](qreal) { update(); });
    auto *list = new QListView(this);
    list->setMouseTracking(true);
    list->installEventFilter(this);
    list->setUniformItemSizes(true);
    list->setTextElideMode(Qt::ElideMiddle);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setView(list);
    // Avoid platform menu delegates overriding the popup's item spacing/colors.
    setItemDelegate(new SelectDelegate(this,list));
    setMaxVisibleItems(8);
}

void ComboBox::paintEvent(QPaintEvent *event) {
    QComboBox::paintEvent(event);
    QStyleOptionComboBox option;
    initStyleOption(&option);
    const QRect arrow = style()->subControlRect(QStyle::CC_ComboBox, &option,
                                                QStyle::SC_ComboBoxArrow, this);
    const QColor ink(isEnabled() ? Theme::textSecondary() : Theme::textMuted());
    QPainter painter(this);
    const QRect bounds(arrow.center().x() - 7, arrow.center().y() - 7, 14, 14);
    painter.translate(bounds.center()); painter.rotate(180*m_arrow->value());
    Theme::coloredIcon("chevron-down",ink,14).paint(&painter,QRect(-7,-7,14,14));
}

void ComboBox::showPopup() { QComboBox::showPopup(); m_arrow->to(view()->isVisible()?1:0,isVisible()); }
void ComboBox::hidePopup() { QComboBox::hidePopup(); m_arrow->to(0,isVisible()); }
bool ComboBox::isAnimating() const { return m_arrow->running(); }
bool ComboBox::eventFilter(QObject *watched,QEvent *event) {
    if (watched==view() && (event->type()==QEvent::Show || event->type()==QEvent::Hide))
        m_arrow->to(event->type()==QEvent::Show?1:0,isVisible());
    return QComboBox::eventFilter(watched,event);
}
