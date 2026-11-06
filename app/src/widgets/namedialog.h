#pragma once

#include <QDialog>

class QLineEdit;

// The one-time "What should I call you?" question. Save accepts, Skip (or Esc) rejects.
class NameDialog : public QDialog {
    Q_OBJECT

public:
    explicit NameDialog(const QString &suggestion, QWidget *parent = nullptr);
    QString name() const;   // trimmed and capped; empty when nothing was entered

private:
    QLineEdit *m_input = nullptr;
};
