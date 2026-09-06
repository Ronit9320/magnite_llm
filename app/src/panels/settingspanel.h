#pragma once

#include "panels/basepanel.h"

class QComboBox;
class QCheckBox;
class QLineEdit;
class QSpinBox;
class QLabel;
class QPushButton;

// Settings panel: app-level configuration with persistence via QSettings.
// This is the canonical owner of global settings (engine, data dirs, keys).
class SettingsPanel : public BasePanel {
    Q_OBJECT

public:
    explicit SettingsPanel(QWidget *parent = nullptr);

signals:
    void settingsChanged();

private slots:
    void onSave();
    void onBrowseDataDir();

private:
    void setupUi();
    void loadFromSettings();
    void setStatus(const QString &text);

    QComboBox *m_engineOverride;
    QLineEdit *m_dataDir;
    QLineEdit *m_hostedEndpoint;
    QLineEdit *m_hostedKey;
    QCheckBox *m_telemetryOptIn;
    QPushButton *m_saveButton;
    QLabel *m_statusLabel;
};
