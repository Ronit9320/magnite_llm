#include "mainwindow.h"
#include "backend_manager.h"
#include "backend_client.h"
#include "panels/basepanel.h"
#include "panels/modelspanel.h"
#include "panels/trainingpanel.h"
#include "panels/ragpanel.h"
#include "panels/adapterspanel.h"
#include "panels/chatpanel.h"
#include "panels/settingspanel.h"
#include "theme.h"

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
#include <QMessageBox>
#include <QPropertyAnimation>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QVariant>

#include <memory>

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , m_stack(new QStackedWidget(this))
    , m_backend(new BackendManager(this))
    , m_backendClient(new BackendClient(this))
    , m_modelsPanel(nullptr)
    , m_trainingPanel(nullptr)
    , m_ragPanel(nullptr)
    , m_adaptersPanel(nullptr)
    , m_chatPanel(nullptr)
    , m_settingsPanel(nullptr)
    , m_statusPill(nullptr)
    , m_crossfade(nullptr)
    , m_skipFade(false)
    , m_selfTest(false)
{
    setupUi();
    setWindowTitle("Magnite LLM");
    resize(1200, 800);
    setMinimumSize(Theme::minWindowWidth(), Theme::minWindowHeight());

    connect(m_backend, &BackendManager::started, this, &MainWindow::onBackendStarted);
    connect(m_backend, &BackendManager::errorOccurred, this, &MainWindow::onBackendError);

    loadPanelSettings();
    startBackend();
}

MainWindow::~MainWindow() {
    savePanelSettings();
}

void MainWindow::setupUi() {
    auto *central = new QWidget(this);
    // Flat black base; the glass side panels render transparent over it.
    // The background lives in the global QSS (#appRoot -> @bgVoid) so no
    // widget-level stylesheet cascades color onto the child controls.
    central->setObjectName("appRoot");
    auto *mainLayout = new QHBoxLayout(central);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // ---- Sidebar (design doc §5) ----
    auto *sidebar = new QWidget();
    sidebar->setObjectName("sidebar");
    sidebar->setFixedWidth(Theme::sidebarWidthExpanded());
    auto *sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(Theme::spacingBase(), 16,
                                      Theme::spacingBase(), 16);
    sidebarLayout->setSpacing(2);

    auto *logoLabel = new QLabel("Magnite", sidebar);
    logoLabel->setObjectName("logoLabel");
    sidebarLayout->addWidget(logoLabel);

    struct NavInfo {
        QString label;
        QString icon;
        int panel;
    };

    auto makeNavButton = [this](const NavInfo &info) {
        auto *btn = new QPushButton(info.label);
        btn->setObjectName("navBtn");
        btn->setCheckable(true);
        btn->setCursor(Qt::PointingHandCursor);
        btn->setProperty("navicon", info.icon);
        btn->setProperty("panel", info.panel);
        connect(btn, &QPushButton::clicked, this, [this, info]() {
            switchPanel(info.panel);
        });
        m_sideButtons.append(btn);
        return btn;
    };

    // 5 nav items in stack order; Settings is pinned to the bottom.
    const QList<NavInfo> navs = {
        {"Models",   "database",       PanelModels},
        {"Training", "graduation-cap", PanelTraining},
        {"RAG",      "folder",         PanelRag},
        {"Adapters", "layers",         PanelAdapters},
        {"Chat",     "message-circle", PanelChat},
    };
    for (const auto &info : navs)
        sidebarLayout->addWidget(makeNavButton(info));

    sidebarLayout->addStretch();

    // Backend status pill: state property drives the color via QSS.
    m_statusPill = new QLabel("Backend: starting…", sidebar);
    m_statusPill->setObjectName("statusPill");
    m_statusPill->setAlignment(Qt::AlignCenter);
    sidebarLayout->addWidget(m_statusPill);
    connect(m_backend, &BackendManager::started, this, [this]() {
        setPillState("ok", "Backend: connected");
    });
    connect(m_backend, &BackendManager::errorOccurred, this,
            [this](QProcess::ProcessError) {
        setPillState("err", "Backend: error");
    });

    sidebarLayout->addWidget(makeNavButton({"Settings", "settings", PanelSettings}));

    mainLayout->addWidget(sidebar);

    // ---- Panels ----
    m_modelsPanel = new ModelsPanel();
    m_trainingPanel = new TrainingPanel();
    m_ragPanel = new RagPanel();
    m_adaptersPanel = new AdaptersPanel();
    m_chatPanel = new ChatPanel();
    m_settingsPanel = new SettingsPanel();

    m_stack->addWidget(m_modelsPanel);
    m_stack->addWidget(m_trainingPanel);
    m_stack->addWidget(m_ragPanel);
    m_stack->addWidget(m_adaptersPanel);
    m_stack->addWidget(m_chatPanel);
    m_stack->addWidget(m_settingsPanel);

    connect(m_settingsPanel, &SettingsPanel::settingsChanged, this, &MainWindow::savePanelSettings);

    mainLayout->addWidget(m_stack, 1);

    setCentralWidget(central);

    refreshNavIcons();
}

void MainWindow::switchPanel(int index) {
    // Persist whichever panel we're leaving, then show the new one. The nav
    // buttons mirror the active panel (also on programmatic switches, e.g.
    // selftest), then the incoming panel crossfades in.
    m_stack->setCurrentIndex(index);
    for (auto *btn : m_sideButtons)
        btn->setChecked(btn->property("panel").toInt() == index);
    refreshNavIcons();
    startPanelFade();
    savePanelSettings();
}

void MainWindow::refreshNavIcons() {
    // Stroke-tinted icons: blue on the active item, secondary when idle.
    for (auto *btn : m_sideButtons) {
        const QString name = btn->property("navicon").toString();
        const QColor color = btn->isChecked()
            ? QColor(Theme::accentPrimary())
            : QColor(Theme::textSecondary());
        btn->setIcon(Theme::coloredIcon(name, color, 18));
        btn->setIconSize(QSize(18, 18));
    }
}

void MainWindow::startPanelFade() {
    // 150ms fade-in of the destination panel (design doc §6). Skipped in
    // selftest mode so the panel navigation stays deterministic, and the
    // effect is removed on completion so Qt doesn't keep an offscreen buffer.
    if (m_selfTest || m_skipFade)
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
    m_crossfade->setDuration(150);
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
    m_skipFade = true;
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
        const QPixmap pm = this->grab();
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
    m_backendClient->setBaseUrl(m_backend->baseUrl());
    m_modelsPanel->setBackendClient(m_backendClient);
    m_trainingPanel->setBackendClient(m_backendClient);
    m_ragPanel->setBackendClient(m_backendClient);
    m_adaptersPanel->setBackendClient(m_backendClient);
    m_chatPanel->setBackendClient(m_backendClient);
    connect(m_modelsPanel, &ModelsPanel::modelsChanged, m_chatPanel, &ChatPanel::refreshModels);
    connect(m_modelsPanel, &ModelsPanel::modelsChanged,
            m_trainingPanel, &TrainingPanel::refreshOptions);
    connect(m_trainingPanel, &TrainingPanel::trainingCompleted,
            m_adaptersPanel, &AdaptersPanel::refresh);
    connect(m_adaptersPanel, &AdaptersPanel::adapterActivated,
            m_chatPanel, &ChatPanel::refreshEngineStatus);
    statusBar()->showMessage(QString("Backend connected (%1)").arg(m_backend->baseUrl()), 4000);
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
    QMessageBox::warning(this, "Backend Error", msg);
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

        // Exercise every panel by switching to it.
        for (int i = 0; i < PanelCount; ++i) {
            switchPanel(i);
        }

        checkC(m_stack->currentIndex() == PanelSettings, "navigated through all panels");
        checkC(m_stack->count() == PanelCount, "panel count");

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
            bool allPersist = s.contains("panels/Models") && s.contains("panels/Training") &&
                              s.contains("panels/RAG") && s.contains("panels/Chat");
            checkC(allPersist, "settings persisted to QSettings");
        }

        // Backend IPC check: GET /models should return 200 once connected.
        // Phase selftests (training/adapter/pull/inference) depend on the
        // backend being up, so they wait for readiness first.
        const bool phaseTest =
            qEnvironmentVariableIsSet("MAGNITE_SELFTEST_TRAINING") ||
            qEnvironmentVariableIsSet("MAGNITE_SELFTEST_ADAPTER") ||
            qEnvironmentVariableIsSet("MAGNITE_SELFTEST_PULL") ||
            qEnvironmentVariableIsSet("MAGNITE_SELFTEST_INFERENCE") ||
            qEnvironmentVariableIsSet("MAGNITE_SELFTEST_RAG");

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
                        finish();
                    }
                });
            });
        };

        if (phaseTest) {
            waitForBackend(failCount, finish, beginIpcStage);
        } else if (m_backendClient && m_backendClient->isReady()) {
            beginIpcStage();
        } else {
            fprintf(stderr, "[selftest] ok: backend not ready, skipping IPC check\n");
            finish();
        }
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
            body["quantization"] = "4bit";
            body["epochs"] = 3;
            body["max_steps"] = 3;
            body["learning_rate"] = 2e-4;
            body["batch_size"] = 1;
            body["warmup_ratio"] = 0.03;
            body["max_length"] = 64;

            m_backendClient->post("/training/start", body,
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
            body["quantization"] = "4bit";
            body["epochs"] = 3;
            body["max_steps"] = 3;
            body["learning_rate"] = 2e-4;
            body["batch_size"] = 1;
            body["warmup_ratio"] = 0.03;
            body["max_length"] = 64;

            m_backendClient->post("/training/start", body,
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
                                    bool gone = true;
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