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
    // Wide enough for the label exactly as it is painted (icon, 6px gap, text, style padding).
    QSize sizeHint() const override;
    // True when the full label fits on one line (or the button wraps it), so nothing is shown as "Organi…".
    bool textFits() const;
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
