#pragma once

#include <QWidget>
#include <QTimer>
#include <QElapsedTimer>

// Studio 02 registration grid or kinetic hero graphic (kineticGlyph property).
// Existing API names keep callers compatible; motion respects app visibility.
class Starfield : public QWidget {
public:
    explicit Starfield(QWidget *parent = nullptr);
    enum Activity { Idle, Training, Chat };
    void setActivity(Activity activity);
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
    Activity m_activity = Idle;
    bool m_motion = true;
    bool m_stars = true;
};
