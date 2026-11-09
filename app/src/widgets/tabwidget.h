#pragma once
#include <QTabWidget>
#include <QTabBar>
class MotionValue;

class SegmentedTabBar : public QTabBar {
    Q_OBJECT
public:
    explicit SegmentedTabBar(QWidget *parent=nullptr);
    bool isAnimating() const;
protected:
    QSize tabSizeHint(int index) const override;
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void hideEvent(QHideEvent *) override;
    void tabLayoutChange() override;
private:
    void selectIndicator(bool animate);
    MotionValue *m_motion;
    QRectF m_from, m_to, m_indicator;
};
class TabWidget : public QTabWidget {
    Q_OBJECT
public:
    explicit TabWidget(QWidget *parent=nullptr);
};
