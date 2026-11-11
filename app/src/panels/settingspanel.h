#pragma once
#include "panels/basepanel.h"
#include <QJsonObject>
#include <functional>

class StorageSummary;
class DiagnosticsSummary;
class QComboBox;
class QCheckBox;
class QLineEdit;
class QLabel;
class QPushButton;
class QStackedWidget;
class QShowEvent;
class UpdateManager;
class BackendClient;
class ChatStore;
class ChatPanel;
class MotionValue;
class QGraphicsOpacityEffect;

class SettingsPanel : public BasePanel {
    Q_OBJECT
  public:
    explicit SettingsPanel(QWidget* parent = nullptr);
    void setUpdateManager(UpdateManager* manager);
    void setBackendClient(BackendClient* client);
    void setChatContext(ChatStore* store, ChatPanel* chat);
    void setRuntimeLogs(std::function<QString()> logs) { m_runtimeLogs = std::move(logs); }
    void selectCategory(int index);
    void refreshDetails();
  signals:
    void settingsChanged();
    void closeRequested();
    void modelsRequested();
    void notificationPreviewRequested();

  protected:
    void showEvent(QShowEvent* event) override;
    void changeEvent(QEvent* event) override;

  private:
    void setupUi();
    void loadFromSettings();
    void onSave();
    void onBrowseDataDir();
    void setStatus(const QString& text);
    void saveImmediate();
    void resetPreferences();
    void exportChats();
    void deleteChats();
    void reviewReport();
    void viewLogs();
    void showShortcuts();
    void refreshNavigation();
    void showText(const QString& title, const QString& text, bool exportable = false);
    bool writeJson(const QString& title, const QJsonObject& object);
    QComboBox* m_engineOverride = nullptr;
    QLineEdit* m_dataDir = nullptr;
    QCheckBox* m_grid = nullptr;
    QCheckBox* m_reducedMotion = nullptr;
    QLineEdit* m_profileName = nullptr;
    QComboBox* m_meterStyle = nullptr;
    QComboBox* m_autoCompact = nullptr;
    QComboBox* m_theme = nullptr;
    QComboBox* m_scale = nullptr;
    QComboBox* m_sendKey = nullptr;
    QCheckBox* m_trainingNotice = nullptr;
    QCheckBox* m_downloadNotice = nullptr;
    QCheckBox* m_sound = nullptr;
    QCheckBox* m_automatic = nullptr;
    QPushButton* m_saveButton = nullptr;
    QPushButton* m_storageSave = nullptr;
    QPushButton* m_deleteChats = nullptr;
    QLabel* m_statusLabel = nullptr;
    QLabel* m_updateStatus = nullptr;
    QPushButton* m_updateCheck = nullptr;
    QPushButton* m_updateDownload = nullptr;
    QPushButton* m_updateInstall = nullptr;
    QPushButton* m_updateCancel = nullptr;
    QStackedWidget* m_pages = nullptr;
    QLabel* m_title = nullptr;
    StorageSummary* m_storageInfo = nullptr;
    QLabel* m_cachePath = nullptr;
    QLabel* m_effectivePath = nullptr;
    DiagnosticsSummary* m_diagnostics = nullptr;
    QLabel* m_chatCount = nullptr;
    QList<QPushButton*> m_navigation;
    MotionValue* m_pageMotion = nullptr;
    QGraphicsOpacityEffect* m_pageEffect = nullptr;
    BackendClient* m_client = nullptr;
    ChatStore* m_store = nullptr;
    ChatPanel* m_chat = nullptr;
    UpdateManager* m_updater = nullptr;
    QJsonObject m_info;
    QString m_activeDataPath;
    QString m_cacheDirectory;
    std::function<QString()> m_runtimeLogs;
    bool m_loading = false;
    bool m_detailsPending = false;
};
