#pragma once

#include <QLayout>

#include <QList>

// A wrapping layout: children flow left→right and wrap to the next row when
// the available width runs out (Phase 5.5.3 card grid). Widgets keep their
// preferred width; rows stack vertically inside the parent scroll area.
class FlowLayout : public QLayout {
public:
    explicit FlowLayout(QWidget *parent = nullptr, int hSpacing = -1, int vSpacing = -1);
    ~FlowLayout() override;

    // Other card workspaces keep the original two-column geometry.
    void setColumnMinimumWidth(int width) { m_columnMinimumWidth = width; m_maxColumns = 3; invalidate(); }
    void setItemSpacing(int horizontal, int vertical) { m_hSpace = horizontal; m_vSpace = vertical; invalidate(); }
    void setMaximumColumns(int columns) { m_maxColumns = columns; invalidate(); }
    void addItem(QLayoutItem *item) override;
    int count() const override;
    QLayoutItem *itemAt(int index) const override;
    QLayoutItem *takeAt(int index) override;

    bool hasHeightForWidth() const override;
    int heightForWidth(int width) const override;
    QSize sizeHint() const override;
    QSize minimumSize() const override;
    void setGeometry(const QRect &rect) override;

private:
    int doLayout(const QRect &rect, bool testOnly) const;
    QList<QLayoutItem *> m_items;
    int m_hSpace;
    int m_vSpace;
    int m_columnMinimumWidth = 480;
    int m_maxColumns = 2;
};