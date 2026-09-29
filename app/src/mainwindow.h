#pragma once

#include <QMainWindow>
#include <QStackedWidget>
#include <QPushButton>
#include <QVBoxLayout>
#include <QLabel>
#include <QProcess>

#include <QList>
#include <memory>
#include <functional>

class Starfield;
class BackendManager;
class BackendClient;
class BasePanel;
class ModelsPanel;
class TrainingPanel;
class RagPanel;
class AdaptersPanel;
class ChatPanel;
class SettingsPanel;
class QPropertyAnimation;
class QLabel;

class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    void setSelfTestMode(bool enabled) { m_selfTest = enabled; }
    void runSelfTest();
    void captureScreenshots(const QString &dir);

private slots:
    void onBackendStarted();
    void onBackendError(QProcess::ProcessError error);
    void switchPanel(int index);
    void savePanelSettings();

private:
    void chatErrorSelfTest(std::shared_ptr<int> failures, std::function<void()> finish, int scenario = 0);
    void setupUi();
    void startBackend();
    void loadPanelSettings();
    void refreshNavIcons();
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
    void adapterSelfTest(std::shared_ptr<int> failCount, std::function<void()> finish);
    void ragSelfTest(std::shared_ptr<int> failCount, std::function<void()> finish);
    void pollTrainingForAdapter(const QString &jobId, int attempts,
                                std::shared_ptr<int> failCount, std::function<void()> finish);
    void activateAdapterSelfTest(const QString &adapterId,
                                 std::shared_ptr<int> failCount, std::function<void()> finish);
    void waitForBackend(std::shared_ptr<int> failCount, std::function<void()> finish,
                        std::function<void()> proceed, int attempt = 0);

    Starfield *m_background = nullptr;
    QStackedWidget *m_stack;
    BackendManager *m_backend;
    BackendClient *m_backendClient;

    ModelsPanel *m_modelsPanel;
    TrainingPanel *m_trainingPanel;
    RagPanel *m_ragPanel;
    AdaptersPanel *m_adaptersPanel;
    ChatPanel *m_chatPanel;
    SettingsPanel *m_settingsPanel;

    QList<QPushButton*> m_sideButtons;
    QLabel *m_statusPill;
    QLabel *m_connectionDot = nullptr;
    QPropertyAnimation *m_crossfade;
    bool m_skipFade;
    bool m_selfTest;
    QString m_shotDir;

    // Panel indices
    enum Panel { PanelModels, PanelTraining, PanelRag, PanelAdapters, PanelChat, PanelSettings, PanelCount };
};
