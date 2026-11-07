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
    QRect labelRect() const;
    int heightForWidth(int width) const override;
protected:
    void paintEvent(QPaintEvent *) override;
    bool event(QEvent *event) override;
    void enterEvent(QEnterEvent *) override;
    void leaveEvent(QEvent *) override;
    void hideEvent(QHideEvent *) override;
    void changeEvent(QEvent *) override;
private:
    MotionValue *m_hover;
    bool m_truncated=false;
};
