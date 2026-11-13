#pragma once

#include "widgets/settingswindow.h"

class QLineEdit;

// The one-time "What should I call you?" question. Save accepts, Skip (or Esc) rejects.
class NameDialog : public SettingsDetailDialog {
    Q_OBJECT

public:
    explicit NameDialog(const QString &suggestion, QWidget *parent = nullptr);
    QString name() const;   // trimmed and capped; empty when nothing was entered

private:
    QLineEdit *m_input = nullptr;
};
