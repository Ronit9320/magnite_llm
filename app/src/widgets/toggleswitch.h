#pragma once

#include <QCheckBox>

class QVariantAnimation;

// Switch presentation with native checked-state, shortcut and accessibility behavior.
class ToggleSwitch : public QCheckBox {
    Q_OBJECT
public:
    explicit ToggleSwitch(const QString &text, QWidget *parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }
    bool isAnimating() const;
    // Where the label starts (left to right layout), so layout and tests agree with what is painted.
    int labelLeft() const;
protected:
    void paintEvent(QPaintEvent *) override;
    bool hitButton(const QPoint &position) const override;
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void changeEvent(QEvent *event) override;
    void hideEvent(QHideEvent *event) override;
private:
    void settle();
    QVariantAnimation *m_animation;
    qreal m_position = 0;
};
