#pragma once
#include <QDialog>
class SettingsPanel;
class QPropertyAnimation;

// A modal utility window keeps the workspace and its current selection underneath.
class SettingsWindow : public QDialog {
    Q_OBJECT
  public:
    SettingsWindow(SettingsPanel* panel, QWidget* parent);
    void openSettings();
    bool motionRunning() const;
  public slots:
    void done(int result) override;

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void positionWindow();
    void animate(bool closing, int result = Rejected);
    QWidget* m_scrim;
    QPropertyAnimation* m_fade;
    bool m_finishing = false;
    int m_finishResult = Rejected;
};
