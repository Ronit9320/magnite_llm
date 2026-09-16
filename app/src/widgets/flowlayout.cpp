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
    const int h = smartSpacing(const_cast<FlowLayout *>(this), m_hSpace);
    const int v = smartSpacing(const_cast<FlowLayout *>(this), m_vSpace);
    int left = rect.left();
    int top = rect.top();
    int height = 0;
    int x = left;
    // Rows wrap when the next item would overflow the right edge.
    for (QLayoutItem *item : m_items) {
        const int w = item->sizeHint().width();
        const int ih = item->sizeHint().height();
        if (x + w > rect.right() + 1 && x > left) {
            x = left;
            top += height + v;
            height = 0;
        }
        if (!testOnly)
            item->setGeometry(QRect(QPoint(x, top), item->sizeHint()));
        x += w + h;
        height = std::max(height, ih);
    }
    return top + height;
}