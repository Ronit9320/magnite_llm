#include "widgets/combobox.h"
#include "theme.h"

#include <QIcon>
#include <QListView>
#include <QPainter>
#include <QStyleOptionComboBox>
#include <QStyledItemDelegate>

ComboBox::ComboBox(QWidget *parent) : QComboBox(parent) {
    auto *list = new QListView(this);
    list->setUniformItemSizes(true);
    list->setTextElideMode(Qt::ElideMiddle);
    list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    setView(list);
    // Avoid platform menu delegates overriding the popup's item spacing/colors.
    setItemDelegate(new QStyledItemDelegate(list));
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
    Theme::coloredIcon(view()->isVisible() ? "chevron-up" : "chevron-down", ink, 14)
        .paint(&painter, bounds);
}
