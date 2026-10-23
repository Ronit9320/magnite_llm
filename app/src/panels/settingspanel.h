#pragma once

#include "panels/basepanel.h"

class QComboBox;
class QCheckBox;
class QLineEdit;
class QLabel;
class QPushButton;
class UpdateManager;

// Settings panel: app-level configuration with persistence via QSettings.
// This is the canonical owner of global settings (engine, data directory, appearance).
class SettingsPanel : public BasePanel {
    Q_OBJECT

public:
    explicit SettingsPanel(QWidget *parent = nullptr);
    void setUpdateManager(UpdateManager *manager);

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
    QCheckBox *m_grid = nullptr;
    QCheckBox *m_reducedMotion = nullptr;
    QPushButton *m_saveButton;
    QLabel *m_statusLabel;
    QLabel *m_updateStatus = nullptr;
    QPushButton *m_updateCheck = nullptr;
    QPushButton *m_updateDownload = nullptr;
    QPushButton *m_updateInstall = nullptr;
    QPushButton *m_updateCancel = nullptr;
};
