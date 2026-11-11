#pragma once
#include "widgets/button.h"
class ThemeChoice : public Button {
  public:
    ThemeChoice(const QString& mode, QWidget* parent = nullptr);
    QSize sizeHint() const override { return QSize(150, 143); }
    QSize minimumSizeHint() const override { return QSize(118, 143); }
    int heightForWidth(int) const override { return 143; }

  protected:
    void paintEvent(QPaintEvent* event) override;

  private:
    QString m_mode;
};
