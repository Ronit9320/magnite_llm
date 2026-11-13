#pragma once
#include <QDialog>
class SettingsPanel;
class Button;

namespace Ui {
Button *dialogCloseButton(QDialog *dialog, QWidget *parent = nullptr);
}
class QPropertyAnimation;

// A modal utility window keeps the workspace and its current selection underneath.
// Paint the opaque surface explicitly: translucent native windows do not reliably
// receive the default QDialog stylesheet background on every compositor.
class SettingsDetailDialog : public QDialog {
  public:
    explicit SettingsDetailDialog(QWidget* parent = nullptr);

  protected:
    void paintEvent(QPaintEvent* event) override;
};

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
    void paintEvent(QPaintEvent* event) override;

  private:
    void positionWindow();
    void animate(bool closing, int result = Rejected);
    QWidget* m_scrim;
    QPropertyAnimation* m_fade;
    bool m_finishing = false;
    int m_finishResult = Rejected;
};
