#pragma once
#include <QPushButton>
class MotionValue;

// Native button semantics, softly animated theme variants and focus treatment.
class Button : public QPushButton {
    Q_OBJECT
public:
    explicit Button(QWidget *parent=nullptr);
    explicit Button(const QString &text,QWidget *parent=nullptr);
    bool isAnimating() const;
protected:
    void paintEvent(QPaintEvent *) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
    void hideEvent(QHideEvent *) override;
    void changeEvent(QEvent *) override;
private:
    MotionValue *m_hover;
};
