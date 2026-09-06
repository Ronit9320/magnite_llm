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

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>

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
    , m_selfTest(false)
{
    setupUi();
    setWindowTitle("Magnite LLM");
    resize(1200, 800);

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
    auto *mainLayout = new QHBoxLayout(central);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);

    // ---- Sidebar ----
    auto *sidebar = new QWidget();
    sidebar->setObjectName("sidebar");
    sidebar->setStyleSheet(
        "#sidebar { background: #2b2b2b; }"
        "#sidebar QPushButton { text-align: left; padding: 10px 16px; border: none;"
        "   color: #ddd; font-weight: 500; }"
        "#sidebar QPushButton:hover { background: #3a3a3a; }"
        "#sidebar QPushButton:checked { background: #3d6da9; color: white; }"
        "#statusLabel { padding: 10px 16px; color: #aaa; font-size: 12px; }");
    auto *sidebarLayout = new QVBoxLayout(sidebar);
    sidebarLayout->setContentsMargins(0, 0, 0, 0);
    sidebarLayout->setSpacing(0);

    auto *logoLabel = new QLabel("Magnite", sidebar);
    logoLabel->setStyleSheet("font-size: 18px; font-weight: bold; color: white; padding: 16px;");
    sidebarLayout->addWidget(logoLabel);

    auto *sep = new QFrame(sidebar);
    sep->setFrameShape(QFrame::HLine);
    sep->setStyleSheet("color: #444;");
    sidebarLayout->addWidget(sep);

    struct PanelInfo {
        QString label;
        Panel panel;
    };

    const QList<PanelInfo> panels = {
        {"Models", PanelModels},
        {"Training", PanelTraining},
        {"RAG", PanelRag},
        {"Adapters", PanelAdapters},
        {"Chat", PanelChat},
        {"Settings", PanelSettings},
    };

    for (const auto &info : panels) {
        auto *btn = new QPushButton(info.label, sidebar);
        btn->setCheckable(true);
        btn->setChecked(info.panel == PanelModels);
        connect(btn, &QPushButton::clicked, this, [this, btn, info]() {
            switchPanel(info.panel);
            for (auto *other : m_sideButtons) {
                other->setChecked(other == btn);
            }
        });
        sidebarLayout->addWidget(btn);
        m_sideButtons.append(btn);
    }

    sidebarLayout->addStretch();

    auto *statusLabel = new QLabel("Backend: starting...", sidebar);
    statusLabel->setObjectName("statusLabel");
    sidebarLayout->addWidget(statusLabel);
    connect(m_backend, &BackendManager::started, statusLabel, [statusLabel]() {
        statusLabel->setText("Backend: connected");
        statusLabel->setStyleSheet(
            "padding: 10px 16px; color: #7dc27a; font-size: 12px;");
    });
    connect(m_backend, &BackendManager::errorOccurred, statusLabel, [statusLabel]() {
        statusLabel->setText("Backend: error");
        statusLabel->setStyleSheet(
            "padding: 10px 16px; color: #e07a7a; font-size: 12px;");
    });

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
}

void MainWindow::switchPanel(int index) {
    // Persist whichever panel we're leaving, then show the new one.
    m_stack->setCurrentIndex(index);
    savePanelSettings();
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
    m_adaptersPanel->setBackendClient(m_backendClient);
    m_chatPanel->setBackendClient(m_backendClient);
    connect(m_modelsPanel, &ModelsPanel::modelsChanged, m_chatPanel, &ChatPanel::refreshModels);
    connect(m_modelsPanel, &ModelsPanel::modelsChanged,
            m_trainingPanel, &TrainingPanel::refreshOptions);
    connect(m_trainingPanel, &TrainingPanel::trainingCompleted,
            m_adaptersPanel, &AdaptersPanel::refresh);
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
        if (m_backendClient && m_backendClient->isReady()) {
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
                    } else if (qEnvironmentVariableIsSet("MAGNITE_SELFTEST_PULL")) {
                        pullModelSelfTest(failCount, finish);
                    } else if (qEnvironmentVariableIsSet("MAGNITE_SELFTEST_INFERENCE")) {
                        inferenceSelfTest(failCount, finish);
                    } else {
                        finish();
                    }
                });
            });
        } else {
            fprintf(stderr, "[selftest] ok: backend not ready, skipping IPC check\n");
            finish();
        }
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