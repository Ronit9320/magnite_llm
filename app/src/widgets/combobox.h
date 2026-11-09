#pragma once

#include <QComboBox>

// Native combo interaction with a consistent list popup and theme-aware chevron.
class MotionValue;

class ComboBox : public QComboBox {
public:
    explicit ComboBox(QWidget *parent = nullptr);
    void showPopup() override;
    void hidePopup() override;
    bool isAnimating() const;

protected:
    void paintEvent(QPaintEvent *event) override;
    bool eventFilter(QObject *watched,QEvent *event) override;
private:
    MotionValue *m_arrow;
};
