#pragma once

#include <QMainWindow>
#include <QStackedWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QLabel>
#include <QProcess>

#include <QJsonObject>
#include <QList>
#include <memory>
#include <functional>

#include "notice_tracker.h"

class SettingsWindow;
class QSystemTrayIcon;
class StudioCanvas;
class BackendManager;
class BackendClient;
class LibraryPanel;
class BasePanel;
class ModelsPanel;
class ModelHubPanel;
class TrainingPanel;
class TrainingRunPanel;
class RagPanel;
class AdaptersPanel;
class ChatPanel;
class SettingsPanel;
class DocsPanel;
class QPropertyAnimation;
class QLabel;
class UpdateManager;
class ChatStore;
class ChatHistoryList;
class Sidebar;
class ChatHistoryController;
class QTemporaryDir;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void setSelfTestMode(bool enabled);
    void runSelfTest();
    void captureScreenshots(const QString &dir);

protected:
    void changeEvent(QEvent *event) override;

private slots:
    void onBackendStarted();
    void onBackendError(QProcess::ProcessError error);
    void onBackendDataDirInUse();
    void switchPanel(int index);
    void savePanelSettings();

private:
    void chatErrorSelfTest(std::shared_ptr<int> failures, std::function<void()> finish, int scenario = 0);
    void setupUi();
    void showWorkspace();
    void startBackend();
    void notifyCompletion(const QString &kind, bool success, bool preview=false);
    // First launch only: asks what to call the user (for the greeting). Never during verification runs.
    void askForNameIfNeeded();
    void loadPanelSettings();
    void startPanelFade();
    void setPillState(const char *state, const QString &text);
    void captureNextPanel(int index);
    void pullModelSelfTest(std::shared_ptr<int> failCount, std::function<void()> finish);
    void pollPullSelfTest(const QString &jobId, int attempts,
                          std::shared_ptr<int> failCount, std::function<void()> finish);
    void inferenceSelfTest(std::shared_ptr<int> failCount, std::function<void()> finish);
    void trainingSelfTest(std::shared_ptr<int> failCount, std::function<void()> finish);
    void pollTrainingSelfTest(const QString &jobId, int attempts,
                              std::shared_ptr<int> failCount, std::function<void()> finish);
    void withSelfTestQuantization(QJsonObject body, std::function<void(const QJsonObject &)> next);
    void adapterSelfTest(std::shared_ptr<int> failCount, std::function<void()> finish);
    void ragSelfTest(std::shared_ptr<int> failCount, std::function<void()> finish);
    void pollTrainingForAdapter(const QString &jobId, int attempts,
                                std::shared_ptr<int> failCount, std::function<void()> finish);
    void activateAdapterSelfTest(const QString &adapterId,
                                 std::shared_ptr<int> failCount, std::function<void()> finish);
    void waitForBackend(std::shared_ptr<int> failCount, std::function<void()> finish,
                        std::function<void()> proceed, int attempt = 0);

    StudioCanvas *m_background = nullptr;
    QStackedWidget *m_stack;
    QStackedWidget *m_startupPages = nullptr;
    QWidget *m_startupPage = nullptr;
    QWidget *m_workspaceHeader = nullptr;
    QList<QPushButton *> m_startupWindowControls;
    QStackedWidget *m_workspacePages = nullptr;
    TrainingRunPanel *m_runPanel = nullptr;
    int m_runReturnPanel = 1;
    BackendManager *m_backend;
    BackendClient *m_backendClient;
    UpdateManager *m_updater;

    ModelHubPanel *m_modelHubPanel = nullptr;
    ModelsPanel *m_modelsPanel;
    LibraryPanel *m_libraryPanel = nullptr;
    TrainingPanel *m_trainingPanel;
    RagPanel *m_ragPanel;
    AdaptersPanel *m_adaptersPanel;
    ChatPanel *m_chatPanel;
    DocsPanel *m_docsPanel = nullptr;
    ChatStore *m_chatStore = nullptr;
    ChatHistoryList *m_history = nullptr;
    ChatHistoryController *m_historyController = nullptr;
    std::unique_ptr<QTemporaryDir> m_chatTemp;   // verification runs keep their chats here, never in the real data folder
    SettingsPanel *m_settingsPanel;
    SettingsWindow *m_settingsWindow=nullptr;
    QSystemTrayIcon *m_notificationTray=nullptr;

    Sidebar *m_sidebar = nullptr;
    QPushButton *m_themeButton=nullptr;
    QLabel *m_statusPill;
    QLabel *m_connectionDot = nullptr;
    QPropertyAnimation *m_crossfade;
    bool m_skipFade;
    bool m_selfTest;
    NoticeTracker m_notices;   // startup repair notices already shown (a restarted backend sends a new list)
    QString m_shotDir;

    // Panel indices
    enum Panel { PanelModels, PanelTraining, PanelRag, PanelAdapters, PanelChat, PanelDocs, PanelHub, PanelSettings, PanelCount };
};
