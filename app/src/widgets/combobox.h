#pragma once

#include <QComboBox>

// Native combo interaction with a consistent list popup and theme-aware chevron.
class MotionValue;

class ComboBox : public QComboBox {
public:
    // Optional per-item text shown after the name in the popup rows only ("Light · ~450 MB RAM"),
    // so the closed box stays short in narrow layouts.
    static constexpr int DetailRole = Qt::UserRole + 64;
    explicit ComboBox(QWidget *parent = nullptr);
    // Widen the popup (never the box) to fit its longest row.
    void fitPopupToRows();
    void showPopup() override;
    void hidePopup() override;
    bool isAnimating() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched,QEvent *event) override;
private:
    MotionValue *m_arrow;
};
