#include "widgets/flowlayout.h"

#include <QWidget>

#include <algorithm>

namespace {
int smartSpacing(QLayout *layout, int spacing) {
    if (spacing >= 0) return spacing;
    return layout->parentWidget() && layout->parentWidget()->layout()
        ? layout->parentWidget()->layout()->spacing() : -1;
}
}  // namespace

FlowLayout::FlowLayout(QWidget *parent, int hSpacing, int vSpacing)
    : QLayout(parent)
    , m_hSpace(hSpacing)
    , m_vSpace(vSpacing) {}

FlowLayout::~FlowLayout() {
    while (QLayoutItem *item = takeAt(0))
        delete item;
}

void FlowLayout::addItem(QLayoutItem *item) {
    m_items.append(item);
    invalidate();
}

int FlowLayout::count() const {
    return m_items.size();
}

QLayoutItem *FlowLayout::itemAt(int index) const {
    return m_items.value(index);
}

QLayoutItem *FlowLayout::takeAt(int index) {
    if (index < 0 || index >= m_items.size()) return nullptr;
    QLayoutItem *item = m_items.takeAt(index);
    invalidate();
    return item;
}

bool FlowLayout::hasHeightForWidth() const {
    return true;
}

int FlowLayout::heightForWidth(int width) const {
    return doLayout(QRect(0, 0, width, 0), true);
}

void FlowLayout::setGeometry(const QRect &rect) {
    QLayout::setGeometry(rect);
    doLayout(rect, false);
}

QSize FlowLayout::sizeHint() const {
    return minimumSize();
}

QSize FlowLayout::minimumSize() const {
    QSize size;
    for (QLayoutItem *item : m_items)
        size = size.expandedTo(item->minimumSize());
    const int h = smartSpacing(const_cast<FlowLayout *>(this), m_hSpace);
    const int v = smartSpacing(const_cast<FlowLayout *>(this), m_vSpace);
    size += QSize(2 * h, 2 * v);
    return size;
}

int FlowLayout::doLayout(const QRect &rect, bool testOnly) const {
    const int gap = std::max(0, smartSpacing(const_cast<FlowLayout *>(this), m_hSpace));
    const int rowGap = std::max(0, smartSpacing(const_cast<FlowLayout *>(this), m_vSpace));
    const QMargins margins = contentsMargins();
    const QRect area = rect.marginsRemoved(margins);
    const int columns = area.width() >= 984 ? 2 : 1;
    const int width = std::max(1, (area.width() - (columns - 1) * gap) / columns);
    int y = area.top();
    int rowHeight = 0;
    int column = 0;
    for (QLayoutItem *item : m_items) {
        if (item->isEmpty()) continue;
        const int height = std::max(item->minimumSize().height(),
            item->hasHeightForWidth() ? item->heightForWidth(width) : item->sizeHint().height());
        if (!testOnly)
            item->setGeometry(QRect(area.left() + column * (width + gap), y, width, height));
        rowHeight = std::max(rowHeight, height);
        if (++column == columns) {
            y += rowHeight + rowGap;
            column = 0;
            rowHeight = 0;
        }
    }
    if (column) y += rowHeight;
    else if (y > area.top()) y -= rowGap;
    return y - rect.top() + margins.bottom();
}
