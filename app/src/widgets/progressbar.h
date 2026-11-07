#pragma once
#include <QProgressBar>
class MotionValue;
class ProgressBar : public QProgressBar {
    Q_OBJECT
public:
    explicit ProgressBar(QWidget *parent=nullptr);
    bool isAnimating() const;
protected:
    void paintEvent(QPaintEvent *) override;
    void hideEvent(QHideEvent *) override;
private:
    MotionValue *m_motion;
};
