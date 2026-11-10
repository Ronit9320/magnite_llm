#include "widgets/clickfeedback.h"
#include "widgets/brandmark.h"
#include <QShortcut>
#include "mainwindow.h"
#include "backend_manager.h"
#include "backend_client.h"
#include "update_manager.h"
#include "update_config.h"
#include "panels/basepanel.h"
#include "panels/modelspanel.h"
#include "panels/trainingpanel.h"
#include "panels/trainingrunpanel.h"
#include "panels/ragpanel.h"
#include "panels/adapterspanel.h"
#include "panels/chatpanel.h"
#include "panels/chathistorycontroller.h"
#include "panels/chatstore.h"
#include "panels/greeting.h"
#include "widgets/chathistorylist.h"
#include "widgets/sidebar.h"
#include "widgets/sidebarstate.h"
#include "widgets/namedialog.h"
#include "panels/settingspanel.h"
#include "widgets/settingswindow.h"
#include "widgets/shortcuts.h"
#include "widgets/brandmark.h"
#include <QSystemTrayIcon>
#include <QShortcut>
#include <QSignalBlocker>
#ifdef MAGNITE_DESKTOP_DBUS
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#endif
#include "theme.h"
#include "widgets/studiocanvas.h"

#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QEasingCurve>
#include <QFile>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include "panels/chatbubble.h"
#include "widgets/overviewtile.h"
#include <QMessageBox>
#include <QPropertyAnimation>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QTemporaryDir>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QHostAddress>
#include <QVariant>
#include <QSaveFile>

#include <memory>

namespace {
// Runs that must never touch the user's real chats or ask questions.
bool verificationRun() {
    const auto args = QCoreApplication::arguments();
    return args.contains("--selftest") || args.contains("--screenshot") || args.contains("--ui-check");
}
}  // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_stack(new QStackedWidget(this))
    , m_backend(new BackendManager(this))
    , m_backendClient(new BackendClient(this))
    , m_updater(new UpdateManager(this))
    , m_modelsPanel(nullptr)
    , m_trainingPanel(nullptr)
    , m_ragPanel(nullptr)
    , m_adaptersPanel(nullptr)
    , m_chatPanel(nullptr)
    , m_settingsPanel(nullptr)
    , m_statusPill(nullptr)
    , m_crossfade(nullptr)
    , m_skipFade(false)
    , m_selfTest(QCoreApplication::arguments().contains("--selftest"))
{
    setupUi();
    setWindowTitle("Magnite");
    setWindowIcon(BrandMark::icon());
    resize(1280, 820);
    setMinimumSize(Theme::minWindowWidth(), Theme::minWindowHeight());

    connect(m_backend, &BackendManager::started, this, &MainWindow::onBackendStarted);
    connect(m_backend, &BackendManager::errorOccurred, this, &MainWindow::onBackendError);
    connect(m_backend, &BackendManager::dataDirInUse, this, &MainWindow::onBackendDataDirInUse);

    loadPanelSettings();
    startBackend();
    m_settingsPanel->setUpdateManager(m_updater);
    connect(m_updater, &UpdateManager::installRequested, this, [this]() {
        if (!m_backendClient->isReady()) {
            QMessageBox::information(this, "Update", "Wait for the local service to become ready before installing."); return;
        }
        if (QMessageBox::question(this, "Install preview update",
            "Restart Magnite to install the update? Finish your work and export anything you need from this conversation first.") != QMessageBox::Yes) return;
        setEnabled(false);
        m_backendClient->post("/updates/quiesce", {}, [this](int status, const QJsonDocument &doc) {
            if (status != 200) {
                setEnabled(true);
                QMessageBox::information(this, "Update postponed", doc.object().value("detail").toString("Could not confirm that the local service is idle. Retry when all work is finished."));
                return;
            }
            m_backendClient->abortAll();
            if (!m_backend->stopForUpdate()) {
                setEnabled(true);
                m_backendClient->post("/updates/resume", {}, [](int, const QJsonDocument &) {});
                QMessageBox::information(this, "Update postponed", "The local service has not finished shutting down. Close and reopen Magnite before retrying.");
                return;
            }
            m_updater->install();
        });
    });
    connect(m_updater, &UpdateManager::restartReady, this, []() { QCoreApplication::quit(); });
    connect(m_updater, &UpdateManager::installFailed, this, [this](const QString &reason) {
        setEnabled(true);
        if (!m_backend->isRunning()) startBackend();
        QMessageBox::information(this, "Update", reason);
    });
    connect(m_backend, &BackendManager::started, this, []() {
        QSettings().setValue("updates/highestSequence", qMax(MAGNITE_UPDATE_SEQUENCE,
            QSettings().value("updates/highestSequence", MAGNITE_UPDATE_SEQUENCE).toInt()));
        const auto args = QCoreApplication::arguments();
        const int index = args.indexOf("--update-health-file");
        if (index >= 0 && index + 1 < args.size()) {
            QSaveFile health(args[index + 1]);
            if (health.open(QIODevice::WriteOnly)) {
                health.write(QCoreApplication::applicationVersion().toUtf8());
                health.commit();
            }
        }
    });
    const auto args = QCoreApplication::arguments();
    const bool verification = m_selfTest || args.contains("--ui-check") || args.contains("--screenshot");
    if (!verification && QSettings().value("updates/checkOnStartup", true).toBool())
        QTimer::singleShot(5000, m_updater, &UpdateManager::check);
    if (!verificationRun()) QTimer::singleShot(0, this, &MainWindow::askForNameIfNeeded);
}

MainWindow::~MainWindow() {
    m_settingsWindow->hide();
    m_chatPanel->flushSave();   // before anything is torn down: the last change must reach disk
    // Settle replies while the panels referenced by their callbacks still exist.
    m_backendClient->abortAll();
    savePanelSettings();
}

void MainWindow::setupUi() {
    auto *central = new QWidget(this);
    // The paper workspace and horizontal masthead share global theme tokens.
    central->setObjectName("appRoot");
    auto *mainLayout = new QHBoxLayout(central);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // ---- Left menu ----
    m_sidebar = new Sidebar({{"Library", "box", PanelModels},
                             {"Training", "graduation-cap", PanelTraining},
                             {"Knowledge", "database", PanelRag},
                             {"Adapters", "layers", PanelAdapters},
                             {"Playground", "message-circle", PanelChat}},
                            {"Settings", "settings", PanelSettings}, PanelChat);
    m_history = m_sidebar->history();
    connect(m_sidebar, &Sidebar::panelRequested, this, &MainWindow::switchPanel);
    auto *brandSeal = m_sidebar->brandSeal();

    auto *mode = new QPushButton("Dark mode", central);
    m_themeButton=mode;
    mode->setObjectName("themeToggle");
    mode->setCheckable(true);
    mode->setChecked(Theme::isDark());
    mode->setText(Theme::isDark() ? "Light mode" : "Dark mode");
    mode->setToolTip("Switch between light and dark appearance");
    mode->setCursor(Qt::PointingHandCursor);
    connect(mode, &QPushButton::toggled, this, [this, mode](bool dark) {
        Theme::setDark(dark);
        QSettings().setValue("appearance/studio3Dark", dark);
        QSettings().setValue("appearance/themeMode", dark?"dark":"light");
        mode->setText(dark ? "Light mode" : "Dark mode");
        m_sidebar->refreshIcons();
    });

    // Quiet, non-interactive service status; details are available on hover.
    auto *connection = new QFrame(central);
    connection->setObjectName("connectionStatus");
    connection->setFixedHeight(28);
    connection->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    auto *connectionLayout = new QHBoxLayout(connection);
    connectionLayout->setContentsMargins(10, 0, 10, 0);
    connectionLayout->setSpacing(7);
    m_connectionDot = new QLabel(connection);
    m_connectionDot->setObjectName("connectionDot");
    m_connectionDot->setFixedSize(6, 6);
    m_statusPill = new QLabel(connection);
    m_statusPill->setObjectName("statusPill");
    connectionLayout->addWidget(m_connectionDot);
    connectionLayout->addWidget(m_statusPill);
    setPillState("starting", "Connecting");

    connect(m_backend, &BackendManager::started, this, [this]() {
        setPillState("ok", "Connected");
    });
    connect(m_backend, &BackendManager::errorOccurred, this,
            [this](QProcess::ProcessError) {
        setPillState("err", "Unavailable");
    });

    mainLayout->addWidget(m_sidebar);

    // ---- Panels ----
    m_modelsPanel = new ModelsPanel();
    m_trainingPanel = new TrainingPanel();
    m_ragPanel = new RagPanel();
    m_adaptersPanel = new AdaptersPanel();
    m_chatPanel = new ChatPanel();
    // Verification runs keep their chats in a throwaway folder unless a data folder was chosen explicitly.
    m_chatTemp = verificationRun() && qEnvironmentVariableIsEmpty("MAGNITE_HOME") ? std::make_unique<QTemporaryDir>() : nullptr;
    m_chatStore = new ChatStore(m_chatTemp ? m_chatTemp->path() + "/chats" : ChatStore::defaultDir(), this);
    m_chatPanel->setChatStore(m_chatStore);
    m_historyController = new ChatHistoryController(m_chatStore, m_chatPanel, m_history, this, this);
    // The strip's + asks exactly like the list's New chat row, and is busy exactly when the row is.
    connect(m_sidebar, &Sidebar::newChatRequested, m_history, &ChatHistoryList::newChatRequested);
    connect(m_chatPanel, &ChatPanel::busyChanged, m_sidebar, &Sidebar::setBusy);
    m_sidebar->setBusy(m_chatPanel->isBusy());
    m_sidebar->setAnimationsEnabled(!verificationRun());
    m_settingsPanel = new SettingsPanel();

    connect(m_modelsPanel,&ModelsPanel::workspaceRequested,this,&MainWindow::switchPanel);

    connect(m_modelsPanel,&ModelsPanel::loadRequested,this,[this](const QString &id) {
        switchPanel(PanelChat);
        m_chatPanel->loadModel(id);
    });
    m_stack->addWidget(m_modelsPanel);
    m_stack->addWidget(m_trainingPanel);
    m_stack->addWidget(m_ragPanel);
    m_stack->addWidget(m_adaptersPanel);
    m_stack->addWidget(m_chatPanel);
    m_settingsWindow=new SettingsWindow(m_settingsPanel,this);
    m_settingsPanel->setBackendClient(m_backendClient);
    m_settingsPanel->setChatContext(m_chatStore,m_chatPanel);
    m_settingsPanel->setRuntimeLogs([this]{return m_backend->recentLogs();});
    connect(m_settingsPanel,&SettingsPanel::modelsRequested,this,[this]{m_settingsWindow->accept();switchPanel(PanelModels);});
    auto *settingsKey=new QShortcut(Shortcuts::settings(),this);
    connect(settingsKey,&QShortcut::activated,this,[this]{switchPanel(PanelSettings);});
    auto *newChatKey=new QShortcut(Shortcuts::newChat(),this);
    connect(newChatKey,&QShortcut::activated,this,[this]{if(!m_chatPanel->isBusy()){switchPanel(PanelChat);QMetaObject::invokeMethod(m_chatPanel,"onClearChat");}});
    m_notificationTray=new QSystemTrayIcon(BrandMark::icon(),this);
    m_notificationTray->setToolTip("Magnite");
    connect(m_notificationTray,&QSystemTrayIcon::activated,this,[this](QSystemTrayIcon::ActivationReason){showNormal();raise();activateWindow();});
    connect(m_modelsPanel,&ModelsPanel::downloadFinished,this,[this](bool success){notifyCompletion("downloads",success);});
    connect(m_trainingPanel,&TrainingPanel::runFinished,this,[this](bool success){notifyCompletion("training",success);});
    connect(m_settingsPanel,&SettingsPanel::notificationPreviewRequested,this,[this]{notifyCompletion("training",true,true);});

    connect(m_settingsPanel,&SettingsPanel::settingsChanged,this,[this,mode]{
        QSignalBlocker block(mode);mode->setChecked(Theme::isDark());mode->setText(Theme::isDark()?"Light mode":"Dark mode");m_sidebar->refreshIcons();
    });
    connect(m_settingsPanel, &SettingsPanel::settingsChanged, this, &MainWindow::savePanelSettings);
    connect(m_settingsPanel, &SettingsPanel::settingsChanged, m_chatPanel, &ChatPanel::reloadContextSettings);
    connect(m_settingsPanel, &SettingsPanel::settingsChanged, this, [this]() {
        if (m_backendClient->isReady()) {
            m_backendClient->post("/inference/settings",
                QJsonObject{{"engine_override", QSettings().value("engine/override", 0).toInt()}},
                [this](int status, const QJsonDocument &) {
                    if (status != 200) onBackendError(QProcess::UnknownError);
                });
        }
    });

    auto *workspace = new QWidget(central);
    auto *workspaceLayout = new QVBoxLayout(workspace);
    workspaceLayout->setContentsMargins(0,0,0,0);
    workspaceLayout->setSpacing(0);
    auto *bar = new QFrame(workspace);
    bar->setObjectName("workspaceBar");
    bar->setFixedHeight(52);
    auto *barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(16,0,16,0);
    auto *breadcrumb = new QLabel("LOCAL WORKSPACE   /   Intelligence, on your terms.",bar);
    breadcrumb->setObjectName("cardMeta");
    barLayout->addWidget(breadcrumb);
    barLayout->addStretch();
    barLayout->setSpacing(12);
    barLayout->addWidget(connection);
    auto *separator = new QFrame(bar);
    separator->setObjectName("headerDivider");
    separator->setFixedSize(1, 16);
    barLayout->addWidget(separator);
    barLayout->addWidget(mode);
    workspaceLayout->addWidget(bar);
    m_background = new StudioCanvas(workspace);
    auto *canvasLayout = new QVBoxLayout(m_background);
    canvasLayout->setContentsMargins(0,0,0,0);
    m_workspacePages=new QStackedWidget(this);
    m_runPanel=new TrainingRunPanel();
    m_workspacePages->addWidget(m_stack); m_workspacePages->addWidget(m_runPanel);
    auto openRun=[this](const QString &id) {
        m_runReturnPanel=m_stack->currentIndex();
        m_workspacePages->setCurrentWidget(m_runPanel); m_runPanel->openRun(id);
    };
    connect(m_trainingPanel,&TrainingPanel::runDetailsRequested,this,openRun);
    connect(m_adaptersPanel,&AdaptersPanel::runDetailsRequested,this,openRun);
    connect(m_runPanel,&TrainingRunPanel::backRequested,this,[this] { switchPanel(m_runReturnPanel); });
    canvasLayout->addWidget(m_workspacePages);
    workspaceLayout->addWidget(m_background,1);
    mainLayout->addWidget(workspace, 1);
    auto applyAppearance = [this, brandSeal]() {
        QSettings settings;
        brandSeal->setMotionEnabled(!settings.value("appearance/studio3ReducedMotion",false).toBool());
        m_background->setGridVisible(settings.value("appearance/studio3Ambient",true).toBool());
        m_background->setMotionEnabled(!settings.value("appearance/studio3ReducedMotion",false).toBool());
        for (auto *widget : m_stack->findChildren<QWidget *>()) {
            if (auto *canvas = dynamic_cast<StudioCanvas *>(widget))
                canvas->setMotionEnabled(!settings.value("appearance/studio3ReducedMotion",false).toBool());
        }
    };
    connect(m_settingsPanel, &SettingsPanel::settingsChanged,this,applyAppearance);
    applyAppearance();

    setCentralWidget(central);
    new ClickFeedback(this);

    m_sidebar->setCurrentPanel(m_stack->currentIndex());
}

void MainWindow::switchPanel(int index) {
    if(index==PanelSettings) {
        m_sidebar->setCurrentPanel(m_stack->currentIndex());
        m_settingsWindow->openSettings();return;
    }
    if(index<0 || index>=m_stack->count())return;
    if(m_settingsWindow->isVisible())m_settingsWindow->reject();
    // Persist whichever panel we're leaving, then show the new one. The nav
    // buttons mirror the active panel (also on programmatic switches, e.g.
    // selftest), then the incoming panel crossfades in.
    m_workspacePages->setCurrentWidget(m_stack);
    m_stack->setCurrentIndex(index);
    m_sidebar->setCurrentPanel(index);
    startPanelFade();
    savePanelSettings();
}

void MainWindow::askForNameIfNeeded() {
    if (Greeting::nameAsked()) return;
    NameDialog dialog(Greeting::accountNameSuggestion(), this);
    if (dialog.exec() == QDialog::Accepted) Greeting::setUserName(dialog.name());
    Greeting::setNameAsked(true);   // Save and Skip both end the question for good
    m_chatPanel->reloadContextSettings();   // a blank chat picks up the name at once
}

void MainWindow::startPanelFade() {
    // Brief fade-in of the destination panel. Skipped in
    // selftest mode so the panel navigation stays deterministic, and the
    // effect is removed on completion so Qt doesn't keep an offscreen buffer.
    if (m_selfTest || m_skipFade || QSettings().value("appearance/studio3ReducedMotion", false).toBool())
        return;

    if (m_crossfade) {
        m_crossfade->stop();
        delete m_crossfade;
        m_crossfade = nullptr;
    }
    m_stack->setGraphicsEffect(nullptr);

    auto *effect = new QGraphicsOpacityEffect(m_stack);
    m_stack->setGraphicsEffect(effect);
    effect->setOpacity(0.0);

    m_crossfade = new QPropertyAnimation(effect, "opacity", this);
    m_crossfade->setDuration(Theme::motionStandardMs());
    m_crossfade->setStartValue(0.0);
    m_crossfade->setEndValue(1.0);
    m_crossfade->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_crossfade, &QPropertyAnimation::finished, this, [this]() {
        if (m_crossfade) {
            m_crossfade->deleteLater();
            m_crossfade = nullptr;
        }
        m_stack->setGraphicsEffect(nullptr);
    });
    m_crossfade->start();
}

void MainWindow::setPillState(const char *state, const QString &text) {
    m_statusPill->setText(text);
    const QString detail = QString::fromLatin1(state) == "ok"
        ? "Magnite’s local service is connected.\n" + m_backend->baseUrl()
        : QString::fromLatin1(state) == "err"
            ? "Magnite’s local service is unavailable. Check the backend error for details."
            : "Starting Magnite’s local service…";
    m_statusPill->parentWidget()->setToolTip(detail);
    m_statusPill->setAccessibleName("Local service: " + text);
    m_connectionDot->setProperty("state", state);
    m_connectionDot->style()->unpolish(m_connectionDot);
    m_connectionDot->style()->polish(m_connectionDot);
    m_connectionDot->update();
    m_statusPill->setProperty("state", state);
    m_statusPill->style()->unpolish(m_statusPill);
    m_statusPill->style()->polish(m_statusPill);
}

void MainWindow::captureScreenshots(const QString &dir) {
    // Dev tool (+ Phase 5.5.7 review aid): captures each panel to
    // <dir>/panel-<index>.png using an in-process widget grab, which works
    // on Wayland where the X root grab comes back black.
    QDir().mkpath(dir);
    m_shotDir = dir;
    // MAGNITE_SCREENSHOT_SIDEBAR=collapsed shows the strip (not stored in Settings).
    if (qEnvironmentVariable("MAGNITE_SCREENSHOT_SIDEBAR") == "collapsed") m_sidebar->setCollapsed(true);
    // MAGNITE_SCREENSHOT_THEME=dark shows the dark theme without storing it.
    if (qEnvironmentVariable("MAGNITE_SCREENSHOT_THEME") == "dark") {
        Theme::setDark(true);
        m_sidebar->refreshIcons();
    }
    m_skipFade = true;
    m_background->setMotionEnabled(false);
    captureNextPanel(0);
}

void MainWindow::captureNextPanel(int index) {
    if (index >= PanelCount) {
        fprintf(stderr, "[screenshot] done\n");
        QCoreApplication::exit(0);
        return;
    }
    switchPanel(index);
    QTimer::singleShot(300, this, [this, index]() {
        const QPixmap pm = index==PanelSettings ? m_settingsWindow->grab() : this->grab();
        const QString path = QStringLiteral("%1/panel-%2.png").arg(m_shotDir).arg(index);
        if (!pm.save(path)) {
            fprintf(stderr, "[screenshot] FAIL: %s\n", qUtf8Printable(path));
            QCoreApplication::exit(1);
            return;
        }
        fprintf(stderr, "[screenshot] ok: %s\n", qUtf8Printable(path));
        captureNextPanel(index + 1);
    });
}

void MainWindow::savePanelSettings() {
    QSettings s;
    QList<BasePanel*> panels = {
        m_modelsPanel, m_trainingPanel, m_ragPanel, m_chatPanel,
    };
    for (auto *p : panels) {
        if (p)
            s.setValue(QString("panels/%1").arg(p->title()), p->settingsJson());
    }
}

void MainWindow::loadPanelSettings() {
    QSettings s;
    QList<BasePanel*> panels = {
        m_modelsPanel, m_trainingPanel, m_ragPanel, m_chatPanel,
    };
    for (auto *p : panels) {
        if (p)
            p->applySettingsJson(s.value(QString("panels/%1").arg(p->title())).toString());
    }
}

void MainWindow::startBackend() {
    m_backend->start();
}

void MainWindow::onBackendStarted() {
    m_backendClient->setAuthToken(m_backend->apiToken());
    m_backendClient->setBaseUrl(m_backend->baseUrl());
    m_modelsPanel->setBackendClient(m_backendClient);
    m_trainingPanel->setBackendClient(m_backendClient);
    m_runPanel->setBackendClient(m_backendClient);
    m_ragPanel->setBackendClient(m_backendClient);
    m_adaptersPanel->setBackendClient(m_backendClient);
    m_chatPanel->setBackendClient(m_backendClient);
    if(m_settingsWindow->isVisible())m_settingsPanel->refreshDetails();
    connect(m_modelsPanel, &ModelsPanel::modelsChanged, m_chatPanel, &ChatPanel::refreshModels);
    connect(m_modelsPanel, &ModelsPanel::modelsChanged,
            m_trainingPanel, &TrainingPanel::refreshOptions);
    connect(m_trainingPanel, &TrainingPanel::trainingCompleted,
            m_adaptersPanel, &AdaptersPanel::refresh);
    connect(m_adaptersPanel, &AdaptersPanel::adapterActivated,
            m_chatPanel, &ChatPanel::refreshEngineStatus);
    // Connection details live in the header; do not create a second status bar.
}

void MainWindow::onBackendDataDirInUse() {
    const QString msg = "Magnite is already running. Another Magnite window is using your data folder; "
                        "switch to it, or quit it and open Magnite again.";
    m_backendClient->abortAll();
    m_backendClient->setBaseUrl(QString());
    m_backendClient->setAuthToken(QString());
    setPillState("error", "Already running");
    if (m_selfTest) fprintf(stderr, "[selftest] backend error: %s\n", qUtf8Printable(msg));
    else QMessageBox::information(this, "Magnite is already running", msg);
}

void MainWindow::onBackendError(QProcess::ProcessError error) {
    QString msg;
    switch (error) {
        case QProcess::FailedToStart:
            msg = "Backend failed to start. Is Python installed?";
            break;
        case QProcess::Crashed:
            msg = "Backend crashed.";
            break;
        default:
            msg = "Backend error.";
    }
    m_backendClient->abortAll();
    m_backendClient->setBaseUrl(QString());
    m_backendClient->setAuthToken(QString());
    setPillState("error", msg);
    m_settingsPanel->refreshDetails();
    if (m_selfTest) fprintf(stderr, "[selftest] backend error: %s\n", qUtf8Printable(msg));
    else QMessageBox::warning(this, "Backend Error", msg);
}

void MainWindow::runSelfTest() {
    auto failCount = std::make_shared<int>(0);

    // A QTimer-gated sequence so async backend checks can complete before exit.
    QTimer::singleShot(0, this, [this, failCount]() {
        const auto finish = [this, failCount]() {
            fprintf(stderr, "[selftest] %s (%d failures)\n",
                    *failCount == 0 ? "PASS" : "FAIL", *failCount);
            fflush(stderr);
            QCoreApplication::exit(*failCount == 0 ? 0 : 1);
        };

        auto checkC = [failCount](bool ok, const char *name) {
            if (!ok) ++(*failCount);
            fprintf(stderr, "[selftest] %s %s\n", ok ? "ok:" : "FAIL:", name);
        };

        auto *mode = findChild<QPushButton *>("themeToggle");
        const bool initialDark = Theme::isDark();
        mode->click();
        checkC(Theme::isDark() != initialDark &&
               QSettings().value("appearance/studio3Dark").toBool() == Theme::isDark(),
               "theme switch applies and persists");
        mode->click();
        checkC(Theme::isDark() == initialDark, "theme switch restores original mode");

        // Overview shortcuts are native buttons: keyboard activation must navigate.
        const auto tiles = m_modelsPanel->findChildren<OverviewTile *>();
        checkC(tiles.size() == 4, "four accessible overview shortcuts");
        for (auto *tile : tiles) {
            switchPanel(PanelModels);
            QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
            QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
            QApplication::sendEvent(tile, &press);
            QApplication::sendEvent(tile, &release);
            checkC(m_stack->currentIndex() == tile->panel(), "Space activates overview shortcut");
        }
        switchPanel(PanelChat);
        QMetaObject::invokeMethod(m_chatPanel, "onClearChat", Qt::DirectConnection);
        auto *emptyChat = m_chatPanel->findChild<QLabel *>("emptyConversation");
        checkC(emptyChat && emptyChat->isVisibleTo(m_chatPanel), "clearing chat restores empty guidance");

        auto *composer = m_chatPanel->findChild<QPlainTextEdit *>("chatInput");
        composer->setPlainText("First line");
        auto cursor = composer->textCursor();
        cursor.movePosition(QTextCursor::End);
        composer->setTextCursor(cursor);
        QKeyEvent newline(QEvent::KeyPress, Qt::Key_Return, Qt::ShiftModifier, "\n");
        QApplication::sendEvent(composer, &newline);
        checkC(composer->toPlainText() == "First line\n", "Shift+Enter inserts composer newline");
        composer->clear();
        QKeyEvent send(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier, "\n");
        QApplication::sendEvent(composer, &send);
        checkC(composer->toPlainText().isEmpty(), "Enter does not insert composer newline");
        auto *chatSettings = m_chatPanel->findChild<QWidget *>("chatSettings");
        auto *settingsToggle = m_chatPanel->findChild<QPushButton *>("chatSettingsToggle");
        checkC(chatSettings->isHidden(), "chat model settings collapsed by default");
        settingsToggle->click();
        checkC(!chatSettings->isHidden(), "chat model settings expand");
        settingsToggle->click();
        QWidget bubbleHost;
        bubbleHost.resize(700, 400);
        ChatBubble oneLine(ChatBubble::Assistant, "First line", &bubbleHost);
        ChatBubble twoLines(ChatBubble::Assistant, "First line\nSecond line", &bubbleHost);
        checkC(twoLines.height() > oneLine.height(), "chat bubbles respect explicit newlines");

        // Exercise every panel by switching to it.
        for (int i = 0; i < PanelCount; ++i) {
            switchPanel(i);
        }

        checkC(m_settingsWindow->isVisible() && m_stack->currentIndex()==PanelChat, "settings opens as a window above the current workspace");
        checkC(m_stack->count() == PanelCount-1, "five workspaces plus a settings window");

        switchPanel(PanelTraining);
        m_trainingPanel->runDetailsRequested("selftest-missing-run");
        checkC(m_workspacePages->currentWidget()==m_runPanel,"training opens contextual run page");
        m_runPanel->backRequested();
        checkC(m_workspacePages->currentWidget()==m_stack && m_stack->currentIndex()==PanelTraining,"run review returns to training");
        switchPanel(PanelAdapters);
        m_adaptersPanel->runDetailsRequested("selftest-missing-run");
        checkC(m_workspacePages->currentWidget()==m_runPanel,"adapters opens contextual run page");
        switchPanel(PanelSettings);
        checkC(m_workspacePages->currentWidget()==m_runPanel && m_settingsWindow->isVisible(),"settings preserves the contextual run review underneath");

        // Left menu: collapse button, the + under Playground and the busy state.
        {
            const SidebarState storedSidebar = SidebarState::load();
            m_sidebar->setAnimationsEnabled(false);
            m_sidebar->setCollapsed(false);   // start from expanded whatever was stored
            auto *toggle = m_sidebar->findChild<QPushButton *>("sidebarToggle");
            auto *plus = m_sidebar->findChild<QPushButton *>("sidebarNewChat");
            auto *listNew = m_history->findChild<QPushButton *>("historyNew");
            checkC(toggle && plus && listNew && m_sidebar->width() == SidebarState::kExpandedWidth, "sidebar: the real window has the toggle, the + and the New chat row");
            if (toggle && plus && listNew) {
                toggle->click();
                checkC(m_sidebar->isCollapsed() && m_sidebar->width() == 52, "sidebar: the toggle collapses the real menu to 52px");
                switchPanel(PanelChat);
                checkC(plus->isVisibleTo(m_sidebar) && !m_history->isVisibleTo(m_sidebar), "sidebar: collapsed on Playground shows the + and hides the chat list");
                emit m_chatPanel->busyChanged(true);
                checkC(!plus->isEnabled() && !listNew->isEnabled(), "sidebar: a busy chat turns off the + and the New chat row");
                emit m_chatPanel->busyChanged(false);
                checkC(plus->isEnabled() && listNew->isEnabled(), "sidebar: an idle chat turns them back on");
                int asked = 0;
                const auto spy = connect(m_history, &ChatHistoryList::newChatRequested, this, [&asked]() { ++asked; });
                plus->click();
                disconnect(spy);
                checkC(asked == 1, "sidebar: the + sends the same new-chat request as the list's New chat row");
                checkC(m_chatPanel->currentChatId().isEmpty(), "sidebar: the + leaves a blank new chat");
                toggle->click();
                checkC(!m_sidebar->isCollapsed() && m_sidebar->width() == SidebarState::kExpandedWidth && m_history->isVisibleTo(m_sidebar), "sidebar: the toggle brings the real menu back");
            }
            storedSidebar.save();   // leave the user's stored choice as it was
            switchPanel(PanelSettings);
        }

        // Settings round-trips.
        const QList<BasePanel*> panels = {
            m_modelsPanel, m_trainingPanel, m_ragPanel, m_chatPanel};
        for (BasePanel *p : panels) {
            if (!p) continue;
            const QString before = p->settingsJson();
            p->applySettingsJson(before);
            checkC(p->settingsJson() == before,
                   QString("settings round-trip (%1)").arg(p->title()).toUtf8().constData());
        }

        // Persist and reload: values must survive savePanelSettings -> QSettings.
        {
            QSettings s;
            savePanelSettings();
            s.sync();
            bool allPersist = true;
            for (const auto *panel : panels)
                allPersist = allPersist && s.contains("panels/" + panel->title());
            checkC(allPersist, "settings persisted to QSettings");
        }

        // Backend IPC check: GET /models should return 200 once connected.
        // Phase selftests (training/adapter/pull/inference) depend on the
        // backend being up, so they wait for readiness first.
        auto beginIpcStage = [this, failCount, finish]() {
            m_backendClient->get("/models", [this, failCount, finish](int status, const QJsonDocument &doc) {
                fprintf(stderr, "[selftest] %s GET /models status=%d, count=%d\n",
                        status == 200 ? "ok:" : "FAIL:",
                        status, static_cast<int>(doc.array().size()));
                if (status != 200) ++(*failCount);

                // Inference status check
                m_backendClient->get("/inference/status", [this, failCount, finish](int s2, const QJsonDocument &d2) {
                    fprintf(stderr, "[selftest] %s GET /inference/status status=%d, engine=%s\n",
                            s2 == 200 ? "ok:" : "FAIL:",
                            s2, qUtf8Printable(d2.object().value("engine").toObject().value("name").toString()));
                    if (s2 != 200) ++(*failCount);

                    if (qEnvironmentVariableIsSet("MAGNITE_SELFTEST_TRAINING")) {
                        trainingSelfTest(failCount, finish);
                    } else if (qEnvironmentVariableIsSet("MAGNITE_SELFTEST_ADAPTER")) {
                        adapterSelfTest(failCount, finish);
                    } else if (qEnvironmentVariableIsSet("MAGNITE_SELFTEST_PULL")) {
                        pullModelSelfTest(failCount, finish);
                    } else if (qEnvironmentVariableIsSet("MAGNITE_SELFTEST_INFERENCE")) {
                        inferenceSelfTest(failCount, finish);
                    } else if (qEnvironmentVariableIsSet("MAGNITE_SELFTEST_RAG")) {
                        ragSelfTest(failCount, finish);
                    } else {
                        chatErrorSelfTest(failCount, finish);
                    }
                });
            });
        };

        waitForBackend(failCount, finish, beginIpcStage);
    });
}

void MainWindow::chatErrorSelfTest(std::shared_ptr<int> failures,
                                  std::function<void()> finish, int scenario) {
    if (scenario == 4) { finish(); return; }
    auto *probe = new ChatPanel(this);
    probe->hide();
    auto *server = new QTcpServer(probe);
    if (!server->listen(QHostAddress::LocalHost, 0)) {
        ++*failures;
        probe->deleteLater();
        finish();
        return;
    }
    connect(server, &QTcpServer::newConnection, probe, [server, scenario, probe]() {
        while (auto *socket = server->nextPendingConnection()) {
            auto data = std::make_shared<QByteArray>();
            connect(socket, &QTcpSocket::readyRead, probe, [socket, data, scenario]() {
                data->append(socket->readAll());
                if (!data->contains("\r\n\r\n") || socket->property("answered").toBool()) return;
                socket->setProperty("answered", true);
                const bool chat = data->startsWith("POST /inference/chat ");
                if (chat && scenario == 2) { socket->abort(); return; }
                QByteArray body = data->startsWith("GET /models ") ? "[]" : "{}";
                QByteArray status = "200 OK";
                QByteArray type = "application/json";
                if (chat) {
                    if (scenario == 1) { status = "400 Bad Request"; body = "{\"detail\":\"audit HTTP failure\"}"; }
                    else {
                        type = "text/event-stream";
                        body = scenario == 0 ? "data: {\"error\":\"audit SSE failure\"}\n\n"
                                             : "data: {\"delta\":\"partial\"}\n\n";
                    }
                }
                socket->write("HTTP/1.1 " + status + "\r\nContent-Type: " + type +
                              "\r\nContent-Length: " + QByteArray::number(body.size()) +
                              "\r\nConnection: close\r\n\r\n" + body);
                socket->disconnectFromHost();
            });
        }
    });
    auto *client = new BackendClient(probe);
    client->setBaseUrl(QString("http://127.0.0.1:%1").arg(server->serverPort()));
    probe->setBackendClient(client);
    QTimer::singleShot(150, probe, [this, probe, failures, finish, scenario]() {
        probe->findChild<QPlainTextEdit *>("chatInput")->setPlainText("audit question");
        QMetaObject::invokeMethod(probe, "onSend");
        auto *timer = new QTimer(probe);
        auto attempts = std::make_shared<int>(0);
        connect(timer, &QTimer::timeout, probe, [this, probe, timer, attempts, failures, finish, scenario]() {
            const QString status = probe->findChild<QLabel *>("statusLabel")->text();
            auto *send = probe->findChild<QPushButton *>("primaryBtn");
            const bool ready = probe->findChild<QWidget *>("chatActivity")->isHidden() && status.startsWith("Generation ") && status != "Generating...";
            if (!ready && ++*attempts < 100) return;
            timer->stop();
            const bool visible = scenario == 0 ? status.contains("audit SSE failure") :
                                 scenario == 1 ? status.contains("audit HTTP failure") :
                                 scenario == 3 ? status.contains("unexpectedly") : status.contains("error");
            const bool activityStopped = probe->findChild<QWidget *>("chatActivity")->isHidden()
                && send->text() == QStringLiteral("Send  ↑");
            probe->findChild<QPlainTextEdit *>("chatInput")->setPlainText("Retry message");
            const bool ok = ready && visible && activityStopped && send->isEnabled();
            if (!ok) ++*failures;
            fprintf(stderr, "[selftest] %s chat error recovery scenario %d: %s\n",
                    ok ? "ok:" : "FAIL:", scenario, qUtf8Printable(status));
            probe->deleteLater();
            chatErrorSelfTest(failures, finish, scenario + 1);
        });
        timer->start(25);
    });
}

void MainWindow::waitForBackend(std::shared_ptr<int> failCount,
                                std::function<void()> finish,
                                std::function<void()> proceed, int attempt) {
    if (m_backendClient && m_backendClient->isReady()) {
        proceed();
        return;
    }
    if (m_backend && !m_backend->isRunning()) {
        ++(*failCount);
        fprintf(stderr, "[selftest] FAIL: backend process died during startup\n");
        finish();
        return;
    }
    if (attempt > 60) {
        ++(*failCount);
        fprintf(stderr, "[selftest] FAIL: backend never became ready (timeout)\n");
        proceed();
        return;
    }
    QTimer::singleShot(500, this,
        [this, failCount, finish, proceed, attempt]() {
            waitForBackend(failCount, finish, proceed, attempt + 1);
        });
}

void MainWindow::pullModelSelfTest(std::shared_ptr<int> failCount, std::function<void()> finish) {
    QJsonObject body;
    body["ref"] = "sshleifer/tiny-gpt2";
    body["source"] = "huggingface";
    m_backendClient->post("/models/pull", body,
        [this, failCount, finish](int status, const QJsonDocument &doc) {
            if (status != 202 || !doc.object().contains("id")) {
                ++(*failCount);
                fprintf(stderr, "[selftest] FAIL: pull start (status=%d)\n", status);
                finish();
                return;
            }
            const QString jobId = doc.object()["id"].toString();
            fprintf(stderr, "[selftest] ok: pull started job=%s\n",
                    qUtf8Printable(jobId));
            pollPullSelfTest(jobId, 0, failCount, finish);
        });
}

void MainWindow::pollPullSelfTest(const QString &jobId, int attempts,
                                  std::shared_ptr<int> failCount,
                                  std::function<void()> finish) {
    m_backendClient->get("/models/pull/" + jobId,
        [this, jobId, attempts, failCount, finish](int status, const QJsonDocument &doc) {
            if (status != 200) {
                ++(*failCount);
                fprintf(stderr, "[selftest] FAIL: pull poll (status=%d)\n", status);
                finish();
                return;
            }
            const QJsonObject job = doc.object();
            const QString jobStatus = job["status"].toString();
            fprintf(stderr, "[selftest] pull %s: %s\n",
                    qUtf8Printable(jobStatus), qUtf8Printable(job["message"].toString()));
            if (jobStatus == "done") {
                m_backendClient->get("/models", [failCount, finish](int s, const QJsonDocument &d) {
                    const bool ok = (s == 200 && !d.array().isEmpty());
                    if (!ok) ++(*failCount);
                    fprintf(stderr, "[selftest] %s pull e2e: models now registered=%d\n",
                            ok ? "ok:" : "FAIL:", static_cast<int>(d.array().size()));
                    finish();
                });
            } else if (jobStatus == "error") {
                ++(*failCount);
                fprintf(stderr, "[selftest] FAIL: pull job error: %s\n",
                        qUtf8Printable(job["error"].toString()));
                finish();
            } else if (attempts > 120) {
                ++(*failCount);
                fprintf(stderr, "[selftest] FAIL: pull timed out\n");
                finish();
            } else {
                QTimer::singleShot(1000, this, [this, jobId, attempts, failCount, finish]() {
                    pollPullSelfTest(jobId, attempts + 1, failCount, finish);
                });
            }
        });
}

void MainWindow::inferenceSelfTest(std::shared_ptr<int> failCount,
                                   std::function<void()> finish) {
    // Load a tiny GGUF repo through the real /inference/load path, then
    // stream a chat through BackendClient::streamPost and count the deltas.
    const QString ref = "aladar/tiny-random-LlamaForCausalLM-GGUF";
    QJsonObject body;
    body["model"] = ref;
    body["engine"] = "llamacpp";

    m_backendClient->post("/inference/load", body,
        [this, failCount, finish](int loadStatus, const QJsonDocument &doc) {
            if (loadStatus != 200) {
                ++(*failCount);
                fprintf(stderr, "[selftest] FAIL: inference load (status=%d, %s)\n",
                        loadStatus, qUtf8Printable(doc.object()["detail"].toString()));
                finish();
                return;
            }
            fprintf(stderr, "[selftest] ok: inference load engine=%s\n",
                    qUtf8Printable(doc.object()["engine"].toObject()["name"].toString()));

            QJsonObject chatBody;
            chatBody["messages"] = QJsonArray{
                QJsonObject{{"role", "user"}, {"content", "Hello"}}};
            chatBody["max_tokens"] = 8;
            chatBody["temperature"] = 0.7;

            int *deltaCount = new int(0);
            m_backendClient->streamPost("/inference/chat", chatBody,
                [deltaCount](const QJsonObject &event) {
                    if (event.contains("delta")) ++(*deltaCount);
                },
                [this, failCount, finish, deltaCount](int streamStatus) {
                    const int dc = *deltaCount;
                    delete deltaCount;
                    const bool ok = (streamStatus == 200 && dc > 0);
                    if (!ok) ++(*failCount);
                    fprintf(stderr, "[selftest] %s inference e2e: status=%d, deltas=%d\n",
                            ok ? "ok:" : "FAIL:", streamStatus, dc);
                    m_backendClient->post("/inference/unload", QJsonObject(),
                        [this, failCount, finish](int s, const QJsonDocument &) {
                            if (s != 200) ++(*failCount);
                            fprintf(stderr, "[selftest] %s inference unload\n",
                                    s == 200 ? "ok:" : "FAIL:");
                            finish();
                        });
                });
        });
}

void MainWindow::trainingSelfTest(std::shared_ptr<int> failCount,
                                  std::function<void()> finish) {
    // Write a tiny JSONL dataset, ingest it, then run a 3-step QLoRA job and
    // confirm the adapter gets registered via GET /adapters.
    const QString path = QDir::temp().filePath("magnite-selftest-train.jsonl");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        ++(*failCount);
        fprintf(stderr, "[selftest] FAIL: write training dataset\n");
        finish();
        return;
    }
    const char *const lines[] = {
        R"({"text":"The quick brown fox jumps over the lazy dog."})",
        R"({"text":"Magnite trains local language models efficiently."})",
        R"({"text":"LoRA adapters are small and composable."})",
        R"({"text":"Quantum entanglement links distant particles."})",
        R"({"text":"The train station is north of the river."})",
        R"({"text":"Fine-tuning improves model behavior on new data."})",
    };
    for (const char *line : lines) {
        f.write(line);
        f.write("\n", 1);
    }
    f.close();

    QJsonObject ingest;
    ingest["path"] = path;
    m_backendClient->post("/datasets/ingest", ingest,
        [this, failCount, finish](int status, const QJsonDocument &doc) {
            if (status != 201) {
                ++(*failCount);
                fprintf(stderr, "[selftest] FAIL: ingest dataset (status=%d)\n", status);
                finish();
                return;
            }
            const QString datasetId = doc.object()["id"].toString();
            fprintf(stderr, "[selftest] ok: ingested dataset %s\n",
                    qUtf8Printable(datasetId));

            QJsonObject body;
            body["base_model"] = "sshleifer/tiny-gpt2";
            body["dataset_id"] = datasetId;
            body["name"] = "selftest-adapter";
            body["rank"] = 4;
            body["alpha"] = 8;
            body["target_modules"] = "all-linear";
            body["epochs"] = 3;
            body["max_steps"] = 3;
            body["learning_rate"] = 2e-4;
            body["batch_size"] = 1;
            body["warmup_ratio"] = 0.03;
            body["max_length"] = 64;

            withSelfTestQuantization(body, [this, failCount, finish](const QJsonObject &request) {
            m_backendClient->post("/training/start", request,
                [this, failCount, finish](int s2, const QJsonDocument &d2) {
                    if (s2 != 202) {
                        ++(*failCount);
                        fprintf(stderr, "[selftest] FAIL: training start (status=%d)\n", s2);
                        finish();
                        return;
                    }
                    const QString jobId = d2.object()["id"].toString();
                    fprintf(stderr, "[selftest] ok: training started job=%s\n",
                            qUtf8Printable(jobId));
                    pollTrainingSelfTest(jobId, 0, failCount, finish);
                });
            });
        });
}

// QLoRA where the backend can run it (bitsandbytes on Linux), plain LoRA elsewhere.
void MainWindow::withSelfTestQuantization(QJsonObject body,
                                          std::function<void(const QJsonObject &)> next) {
    m_backendClient->get("/training/capabilities",
        [body, next](int status, const QJsonDocument &caps) mutable {
            const bool qlora = status != 200 || caps.object()["quantization"].toBool(true);
            body["quantization"] = qlora ? "4bit" : "none";
            fprintf(stderr, "[selftest] ok: training quantization=%s\n",
                    qUtf8Printable(body["quantization"].toString()));
            next(body);
        });
}

void MainWindow::pollTrainingSelfTest(const QString &jobId, int attempts,
                                      std::shared_ptr<int> failCount,
                                      std::function<void()> finish) {
    m_backendClient->get("/training/status/" + jobId,
        [this, jobId, attempts, failCount, finish](int status, const QJsonDocument &doc) {
            if (status != 200) {
                ++(*failCount);
                fprintf(stderr, "[selftest] FAIL: training poll (status=%d)\n", status);
                finish();
                return;
            }
            const QJsonObject job = doc.object();
            const QString jobStatus = job["status"].toString();
            fprintf(stderr, "[selftest] train %s: %s (step %lld/%lld)\n",
                    qUtf8Printable(jobStatus),
                    qUtf8Printable(job["message"].toString()),
                    job["current_step"].toVariant().toLongLong(),
                    job["total_steps"].toVariant().toLongLong());
            if (jobStatus == "done") {
                m_backendClient->get("/adapters",
                    [this, jobId, failCount, finish](int s, const QJsonDocument &d) {
                        bool found = false;
                        if (s == 200) {
                            for (const auto &v : d.array()) {
                                if (v.toObject()["training_job_id"].toString() == jobId)
                                    found = true;
                            }
                        }
                        if (!found) ++(*failCount);
                        fprintf(stderr, "[selftest] %s training e2e: adapter registered for job\n",
                                found ? "ok:" : "FAIL:");
                        finish();
                    });
            } else if (jobStatus == "error") {
                ++(*failCount);
                fprintf(stderr, "[selftest] FAIL: training job error: %s\n",
                        qUtf8Printable(job["error"].toString()));
                finish();
            } else if (jobStatus == "cancelled") {
                ++(*failCount);
                fprintf(stderr, "[selftest] FAIL: training job cancelled unexpectedly\n");
                finish();
            } else if (attempts > 240) {
                ++(*failCount);
                fprintf(stderr, "[selftest] FAIL: training timed out\n");
                finish();
            } else {
                QTimer::singleShot(1000, this, [this, jobId, attempts, failCount, finish]() {
                    pollTrainingSelfTest(jobId, attempts + 1, failCount, finish);
                });
            }
        });
}

void MainWindow::adapterSelfTest(std::shared_ptr<int> failCount,
                                 std::function<void()> finish) {
    // End-to-end Phase 5: train a tiny QLoRA adapter, activate it for
    // inference (torch/PEFT engine), stream a chat through it, then clear it.
    const QString path = QDir::temp().filePath("magnite-selftest-adapter.jsonl");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        ++(*failCount);
        fprintf(stderr, "[selftest] FAIL: write adapter dataset\n");
        finish();
        return;
    }
    const char *const lines[] = {
        R"({"instruction":"Introduce Magnite","output":"Magnite is a local LLM workstation."})",
        R"({"instruction":"What is QLoRA?","output":"A memory-efficient way to fine-tune large models."})",
        R"({"instruction":"Define an adapter","output":"A small trainable module layered on a frozen base."})",
        R"({"instruction":"Name a reptile","output":"A crocodile basks beside the river."})",
        R"({"instruction":"Pick a color","output":"Teal feels calm and precise."})",
        R"({"instruction":"Sum one and two","output":"One plus two equals three."})",
    };
    for (const char *line : lines) {
        f.write(line);
        f.write("\n", 1);
    }
    f.close();

    QJsonObject ingest;
    ingest["path"] = path;
    m_backendClient->post("/datasets/ingest", ingest,
        [this, failCount, finish](int status, const QJsonDocument &doc) {
            if (status != 201) {
                ++(*failCount);
                fprintf(stderr, "[selftest] FAIL: ingest adapter dataset (status=%d)\n", status);
                finish();
                return;
            }
            const QString datasetId = doc.object()["id"].toString();
            fprintf(stderr, "[selftest] ok: ingested adapter dataset %s\n",
                    qUtf8Printable(datasetId));

            QJsonObject body;
            body["base_model"] = "sshleifer/tiny-gpt2";
            body["dataset_id"] = datasetId;
            body["name"] = "selftest-activate";
            body["rank"] = 4;
            body["alpha"] = 8;
            body["target_modules"] = "all-linear";
            body["epochs"] = 3;
            body["max_steps"] = 3;
            body["learning_rate"] = 2e-4;
            body["batch_size"] = 1;
            body["warmup_ratio"] = 0.03;
            body["max_length"] = 64;

            withSelfTestQuantization(body, [this, failCount, finish](const QJsonObject &request) {
            m_backendClient->post("/training/start", request,
                [this, failCount, finish](int s2, const QJsonDocument &d2) {
                    if (s2 != 202) {
                        ++(*failCount);
                        fprintf(stderr, "[selftest] FAIL: adapter training start (status=%d)\n", s2);
                        finish();
                        return;
                    }
                    const QString jobId = d2.object()["id"].toString();
                    fprintf(stderr, "[selftest] ok: adapter training started job=%s\n",
                            qUtf8Printable(jobId));
                    pollTrainingForAdapter(jobId, 0, failCount, finish);
                });
            });
        });
}

void MainWindow::pollTrainingForAdapter(const QString &jobId, int attempts,
                                        std::shared_ptr<int> failCount,
                                        std::function<void()> finish) {
    m_backendClient->get("/training/status/" + jobId,
        [this, jobId, attempts, failCount, finish](int status, const QJsonDocument &doc) {
            if (status != 200) {
                ++(*failCount);
                fprintf(stderr, "[selftest] FAIL: adapter training poll (status=%d)\n", status);
                finish();
                return;
            }
            const QJsonObject job = doc.object();
            const QString jobStatus = job["status"].toString();
            fprintf(stderr, "[selftest] train %s: %s (step %lld/%lld)\n",
                    qUtf8Printable(jobStatus),
                    qUtf8Printable(job["message"].toString()),
                    job["current_step"].toVariant().toLongLong(),
                    job["total_steps"].toVariant().toLongLong());
            if (jobStatus == "done") {
                m_backendClient->get("/adapters",
                    [this, jobId, failCount, finish](int s, const QJsonDocument &d) {
                        QString adapterId;
                        if (s == 200) {
                            for (const auto &v : d.array()) {
                                if (v.toObject()["training_job_id"].toString() == jobId)
                                    adapterId = v.toObject()["id"].toString();
                            }
                        }
                        if (adapterId.isEmpty()) {
                            ++(*failCount);
                            fprintf(stderr, "[selftest] FAIL: no adapter for training job\n");
                            finish();
                            return;
                        }
                        fprintf(stderr, "[selftest] ok: adapter registered %s\n",
                                qUtf8Printable(adapterId));
                        activateAdapterSelfTest(adapterId, failCount, finish);
                    });
            } else if (jobStatus == "error" || jobStatus == "cancelled") {
                ++(*failCount);
                fprintf(stderr, "[selftest] FAIL: adapter training %s: %s\n",
                        qUtf8Printable(jobStatus),
                        qUtf8Printable(job["error"].toString()));
                finish();
            } else if (attempts > 240) {
                ++(*failCount);
                fprintf(stderr, "[selftest] FAIL: adapter training timed out\n");
                finish();
            } else {
                QTimer::singleShot(1000, this, [this, jobId, attempts, failCount, finish]() {
                    pollTrainingForAdapter(jobId, attempts + 1, failCount, finish);
                });
            }
        });
}

void MainWindow::activateAdapterSelfTest(const QString &adapterId,
                                         std::shared_ptr<int> failCount,
                                         std::function<void()> finish) {
    // Activate via the real /inference/adapter path; the engine should be
    // "torch" and the status should report the active adapter.
    QJsonObject activateBody;
    activateBody["adapter_id"] = adapterId;
    m_backendClient->post("/inference/adapter", activateBody,
        [this, adapterId, failCount, finish](int status, const QJsonDocument &doc) {
            if (status != 200) {
                ++(*failCount);
                fprintf(stderr, "[selftest] FAIL: adapter activate (status=%d, %s)\n",
                        status, qUtf8Printable(doc.object()["detail"].toString()));
                finish();
                return;
            }
            const QJsonObject obj = doc.object();
            const QString engineName = obj["engine"].toObject()["name"].toString();
            const bool ok = (obj["active_adapter_id"].toString() == adapterId &&
                             engineName == "torch");
            if (!ok) ++(*failCount);
            fprintf(stderr, "[selftest] %s adapter activate: engine=%s active=%s\n",
                    ok ? "ok:" : "FAIL:", qUtf8Printable(engineName),
                    qUtf8Printable(obj["active_adapter"].toString()));

            // Stream a chat through the adapter and count token deltas.
            QJsonObject chatBody;
            chatBody["messages"] = QJsonArray{
                QJsonObject{{"role", "user"}, {"content", "Hello from the adapter"}}};
            chatBody["max_tokens"] = 8;
            chatBody["temperature"] = 0.7;

            int *deltaCount = new int(0);
            m_backendClient->streamPost("/inference/chat", chatBody,
                [deltaCount](const QJsonObject &event) {
                    if (event.contains("delta")) ++(*deltaCount);
                },
                [this, adapterId, failCount, finish, deltaCount](int streamStatus) {
                    const int dc = *deltaCount;
                    delete deltaCount;
                    const bool ok = (streamStatus == 200 && dc > 0);
                    if (!ok) ++(*failCount);
                    fprintf(stderr, "[selftest] %s adapter e2e: status=%d, deltas=%d\n",
                            ok ? "ok:" : "FAIL:", streamStatus, dc);

                    // Clear the adapter to leave a clean state.
                    QJsonObject clearBody;
                    clearBody["adapter_id"] = "";
                    m_backendClient->post("/inference/adapter", clearBody,
                        [this, failCount, finish](int s, const QJsonDocument &d) {
                            const bool cleared =
                                (s == 200 && d.object()["active_adapter_id"].toString().isEmpty());
                            if (!cleared) ++(*failCount);
                            fprintf(stderr, "[selftest] %s adapter cleared\n",
                                    cleared ? "ok:" : "FAIL:");
                            finish();
                        });
                });
        });
}

void MainWindow::ragSelfTest(std::shared_ptr<int> failCount,
                             std::function<void()> finish) {
    // Phase 6 e2e: ingest a small document, retrieve it back through the
    // vector store, then delete it and confirm cleanup. (Chat-context
    // injection shares the same store/embedding code paths.)
    const QString path = QDir::temp().filePath("magnite-selftest-rag.txt");
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        ++(*failCount);
        fprintf(stderr, "[selftest] FAIL: write rag document\n");
        finish();
        return;
    }
    f.write("Magnite RAG knowledge base.\n");
    f.write("The Eiffel Tower is a wrought-iron lattice tower in Paris.\n");
    f.write("Retrieval-augmented generation injects matching passages into chat context.\n");
    f.close();

    QJsonObject ingest;
    ingest["path"] = path;
    m_backendClient->post("/rag/documents", ingest,
        [this, failCount, finish](int status, const QJsonDocument &doc) {
            if (status != 201) {
                ++(*failCount);
                fprintf(stderr, "[selftest] FAIL: rag ingest (status=%d, %s)\n",
                        status, qUtf8Printable(doc.object()["detail"].toString()));
                finish();
                return;
            }
            const QString docId = doc.object()["id"].toString();
            const int chunks = doc.object()["chunks"].toInt();
            if (chunks < 1) ++(*failCount);
            fprintf(stderr, "[selftest] %s rag ingest: id=%s chunks=%d\n",
                    chunks >= 1 ? "ok:" : "FAIL:", qUtf8Printable(docId), chunks);

            QJsonObject query;
            query["query"] = "What is the Eiffel Tower made of?";
            query["top_k"] = 3;
            m_backendClient->post("/rag/query", query,
                [this, failCount, finish, docId](int s, const QJsonDocument &d) {
                    bool found = false;
                    double score = 0.0;
                    if (s == 200) {
                        const QJsonArray results = d.object()["results"].toArray();
                        for (const auto &v : results) {
                            if (v.toObject()["document_id"].toString() == docId) {
                                found = true;
                                score = v.toObject()["score"].toDouble();
                                break;
                            }
                        }
                    }
                    if (!found) ++(*failCount);
                    fprintf(stderr, "[selftest] %s rag query: status=%d score=%.3f\n",
                            found ? "ok:" : "FAIL:", s, score);

                    m_backendClient->remove("/rag/documents/" + docId,
                        [this, failCount, finish, docId](int del, const QJsonDocument &) {
                            const bool deleted = (del == 204);
                            if (!deleted) ++(*failCount);
                            fprintf(stderr, "[selftest] %s rag delete\n",
                                    deleted ? "ok:" : "FAIL:");
                            m_backendClient->get("/rag/documents",
                                [this, failCount, finish, docId](int list,
                                                                const QJsonDocument &ld) {
                                    bool gone = (list == 200);
                                    if (list == 200) {
                                        for (const auto &v : ld.array()) {
                                            if (v.toObject()["id"].toString() == docId)
                                                gone = false;
                                        }
                                    }
                                    if (!gone) ++(*failCount);
                                    fprintf(stderr, "[selftest] %s rag cleanup\n",
                                            gone ? "ok:" : "FAIL:");
                                    finish();
                                });
                        });
                });
        });
}

void MainWindow::notifyCompletion(const QString &kind,bool success,bool preview) {
    if(!preview && (verificationRun() || isActiveWindow() || m_settingsWindow->isActiveWindow()))return;
    if(!preview && !QSettings().value("notifications/"+kind,true).toBool())return;
    const QString title=kind=="training"?(success?"Training finished":"Training needs attention"):(success?"Model download finished":"Model download needs attention");
    const QString message=preview?"Test notification — your workspace is ready.":title;
    const bool sound=QSettings().value("notifications/sound",false).toBool();
#ifdef MAGNITE_DESKTOP_DBUS
    QDBusInterface service("org.freedesktop.Notifications","/org/freedesktop/Notifications","org.freedesktop.Notifications",QDBusConnection::sessionBus());
    if(service.isValid()) {
        QVariantMap hints;hints.insert("suppress-sound",!sound);if(sound)hints.insert("sound-name","complete");
        const QList<QVariant> args={QString("Magnite"),uint(0),QString(),QString("Magnite"),message,QStringList(),hints,int(8000)};
        auto *watcher=new QDBusPendingCallWatcher(service.asyncCallWithArgumentList("Notify",args),this);
        connect(watcher,&QDBusPendingCallWatcher::finished,this,[this,preview](QDBusPendingCallWatcher *pending){
            QDBusPendingReply<uint> reply=*pending;
            if(reply.isError()&&preview)QMessageBox::information(m_settingsWindow,"Notifications unavailable","The desktop notification service rejected the request. Check your system notification settings.");
            pending->deleteLater();
        });return;
    }
#endif
    if(!QSystemTrayIcon::isSystemTrayAvailable() || !QSystemTrayIcon::supportsMessages()) {
        if(preview)QMessageBox::information(m_settingsWindow,"Notifications unavailable","This desktop session does not provide notifications. Completion status remains available in the relevant panel.");
        return;
    }
    m_notificationTray->show();
    m_notificationTray->showMessage("Magnite",message,success?QSystemTrayIcon::Information:QSystemTrayIcon::Warning,8000);
    if(sound)QApplication::beep();
}

void MainWindow::changeEvent(QEvent *event) {
    QMainWindow::changeEvent(event);
    if(event->type()==QEvent::ApplicationPaletteChange && m_themeButton && m_sidebar) {
        QSignalBlocker block(m_themeButton);
        m_themeButton->setChecked(Theme::isDark());
        m_themeButton->setText(Theme::isDark()?"Light mode":"Dark mode");
        m_sidebar->refreshIcons();
    }
}
