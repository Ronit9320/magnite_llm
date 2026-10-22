#pragma once

#include <QWidget>
#include <QTimer>
#include <QElapsedTimer>
#include "widgets/logoanimation.h"

// Workspace registration grid or animated brand mark (brandMark property).
// Motion respects reduced-motion preferences and application visibility.
class StudioCanvas : public QWidget {
public:
    explicit StudioCanvas(QWidget *parent = nullptr);
    enum Activity { Idle, Training, Chat };
    void setActivity(Activity activity);
    void setMotionEnabled(bool enabled);
    void setGridVisible(bool visible);
protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
private:
    void updateTimer();
    QTimer m_timer;
    QElapsedTimer m_clock;
    LogoAnimation m_logo;
    QElapsedTimer m_playbackClock;
    qint64 m_playbackMs = 0;
    Activity m_activity = Idle;
    bool m_motion = true;
    bool m_grid = true;
};
