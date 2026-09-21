#pragma once

#include <QWidget>
#include <QTimer>
#include <QElapsedTimer>

// A bounded, deterministic starfield painted behind the workspace. No ML or IO.
class Starfield : public QWidget {
public:
    explicit Starfield(QWidget *parent = nullptr);
    void setMotionEnabled(bool enabled);
    void setStarsVisible(bool visible);
protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
private:
    void updateTimer();
    QTimer m_timer;
    QElapsedTimer m_clock;
    bool m_motion = true;
    bool m_stars = true;
};
