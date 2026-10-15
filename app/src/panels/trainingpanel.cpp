#include <QSplitter>
#include <QTabWidget>
#include "panels/trainingpanel.h"

#include "backend_client.h"
#include "widgets/surface.h"
#include "widgets/studiocanvas.h"
#include "theme.h"
#include <QProgressBar>
#include <QSettings>
#include <QScrollArea>

#include <QCheckBox>
#include "widgets/combobox.h"
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr int kPollMs = 1000;

// After repopulating an editable combo, restore the user's previous text
// (a typed path / repo id) or fall back to the first entry when empty.
void applyPreservedText(QComboBox *combo, const QString &prev) {
    if (!prev.isEmpty()) {
        combo->setCurrentText(prev);
    } else if (combo->count() > 0) {
        combo->setCurrentIndex(0);
    }
}
}  // namespace

TrainingPanel::TrainingPanel(QWidget *parent)
    : BasePanel("Training", parent)
    , m_client(nullptr)
    , m_pollTimer(nullptr)
    , m_lastLoggedStep(-1)
    , m_modelCombo(nullptr)
    , m_datasetCombo(nullptr)
    , m_adapterNameEdit(nullptr)
    , m_hyperBox(nullptr)
    , m_targetModules(nullptr)
    , m_rankSpin(nullptr)
    , m_alphaSpin(nullptr)
    , m_quantCombo(nullptr)
    , m_epochsSpin(nullptr)
    , m_lrSpin(nullptr)
    , m_batchSpin(nullptr)
    , m_warmupSpin(nullptr)
    , m_maxLengthSpin(nullptr)
    , m_qloraCheck(nullptr)
    , m_startButton(nullptr)
    , m_stopButton(nullptr)
    , m_log(nullptr)
    , m_statusLabel(nullptr)
{
    setHeaderSubtitle("Fine-tune a downloaded model locally with LoRA or QLoRA.");
    setupUi();
}

void TrainingPanel::setupUi() {
    auto *columns = new QSplitter(Qt::Horizontal,this);
    columns->setChildrenCollapsible(false);
    columns->setHandleWidth(4);
    contentLayout()->addWidget(columns,1);
    auto *layout = Surface::card(this,"Build your adapter");
    auto *right = new QVBoxLayout();
    right->setSpacing(8);
    auto *setupScroll = new QScrollArea(this);
    setupScroll->setWidgetResizable(true);
    setupScroll->setMinimumWidth(290);
    setupScroll->setWidget(layout->parentWidget());
    columns->addWidget(setupScroll);
    m_setup = setupScroll;
    auto *monitor = new QWidget(this);
    monitor->setLayout(right);
    columns->addWidget(monitor);
    columns->setSizes({350,750});

    m_modelCombo = new ComboBox(this);
    m_modelCombo->setEditable(true);
    m_modelCombo->setMinimumContentsLength(12);
    m_modelCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_modelCombo->setInsertPolicy(QComboBox::NoInsert);
    m_modelCombo->lineEdit()->setPlaceholderText("Choose a model or enter its ID");
    Surface::field(layout, "Base model", m_modelCombo);
    auto *modelRow = new QHBoxLayout();
    auto *refreshBtn = new QPushButton("Refresh", this);
    refreshBtn->setObjectName("secondaryBtn");
    auto *folderBtn = new QPushButton("Folder...", this);
    folderBtn->setObjectName("secondaryBtn");
    modelRow->addWidget(refreshBtn);
    modelRow->addWidget(folderBtn);
    modelRow->addStretch();
    layout->addLayout(modelRow);
    connect(refreshBtn, &QPushButton::clicked, this, &TrainingPanel::refreshOptions);
    connect(folderBtn, &QPushButton::clicked, this, &TrainingPanel::onBrowseBaseModel);

    m_datasetCombo = new ComboBox(this);
    m_datasetCombo->setEditable(true);
    m_datasetCombo->setMinimumContentsLength(12);
    m_datasetCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_datasetCombo->setInsertPolicy(QComboBox::NoInsert);
    m_datasetCombo->lineEdit()->setPlaceholderText("Choose an imported dataset");
    Surface::field(layout, "Dataset", m_datasetCombo);
    auto *datasetRow = new QHBoxLayout();
    auto *ingestBtn = new QPushButton("Import file...", this);
    ingestBtn->setObjectName("secondaryBtn");
    datasetRow->addWidget(ingestBtn);
    datasetRow->addStretch();
    layout->addLayout(datasetRow);
    connect(ingestBtn, &QPushButton::clicked, this, &TrainingPanel::onBrowseDataset);

    Surface::field(layout, "Adapter name", m_adapterNameEdit = new QLineEdit(this));
    m_adapterNameEdit->setPlaceholderText("Auto-named from job if empty");

    setupHyperparameters(this);
    auto *advanced = new QPushButton("Hyperparameters ▾",this);
    advanced->setCheckable(true);
    layout->addWidget(advanced);
    layout->addWidget(m_hyperBox);
    m_hyperBox->hide();
    connect(advanced,&QPushButton::toggled,m_hyperBox,&QWidget::setVisible);
    connect(advanced,&QPushButton::toggled,this,[advanced](bool expanded) {
        advanced->setText(expanded ? "Hyperparameters ▴" : "Hyperparameters ▾");
    });
    auto *defaults = new QLabel("LoRA rank 8 · alpha 16 · 3 epochs by default",this);
    defaults->setObjectName("cardMeta");
    defaults->setWordWrap(true);
    layout->addWidget(defaults);

    m_qloraCheck = new QCheckBox("Use 4-bit QLoRA", this);
    m_qloraCheck->setChecked(true);
    m_qloraCheck->setToolTip("Uses 4-bit NF4 weights. Turn off to choose another precision in Hyperparameters.");
    layout->addWidget(m_qloraCheck);

    auto *savedHint = new QLabel("Completed adapters appear in your Adapters workspace.",this);
    savedHint->setObjectName("cardMeta"); savedHint->setWordWrap(true);
    layout->addWidget(savedHint);

    connect(m_qloraCheck, &QCheckBox::toggled, this, &TrainingPanel::onQuantizationToggled);
    onQuantizationToggled();

    layout->addStretch();
    auto *run = Surface::card(this,"Training session");
    right->addWidget(run->parentWidget());
    m_progressLabel = new QLabel("Ready to train",this);
    m_progressLabel->setObjectName("metricValue");
    auto *session = new QHBoxLayout();
    m_activity = new StudioCanvas(this);
    m_activity->setProperty("brandMark", true);
    m_activity->setProperty("activityIndicator",true);
    m_activity->setObjectName("trainingActivity");
    m_activity->setFixedSize(112,112);
    m_activity->setMotionEnabled(false);
    session->addWidget(m_activity);
    auto *sessionCopy = new QVBoxLayout();
    sessionCopy->setSpacing(8);
    sessionCopy->addStretch();
    sessionCopy->addWidget(m_progressLabel);
    auto *guidance = m_runHint = new QLabel("Choose a model and dataset, then start your run. Follow progress here as your adapter learns.",this);
    guidance->setObjectName("cardMeta");
    guidance->setWordWrap(true);
    guidance->setTextFormat(Qt::PlainText);
    sessionCopy->addWidget(guidance);
    sessionCopy->addStretch();
    session->addLayout(sessionCopy,1);
    run->addLayout(session);
    m_progress = new QProgressBar(this);
    m_progress->setRange(0,100);
    m_progress->setValue(0);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(8);
    run->addWidget(m_progress);
    auto *btns = new QHBoxLayout();
    m_startButton = new QPushButton("Start Training", this);
    m_startButton->setObjectName("primaryBtn");
    m_startButton->setIcon(Theme::coloredIcon("play", QColor(Theme::accentText())));
    m_stopButton = new QPushButton("Stop", this);
    m_stopButton->setObjectName("destructiveBtn");
    m_stopButton->setProperty("themeIcon","circle-stop");
    m_stopButton->setProperty("themeIconTone","error");
    m_stopButton->setIcon(Theme::coloredIcon("circle-stop", QColor(Theme::statusError())));
    m_stopButton->setEnabled(false);
    btns->addWidget(m_startButton);
    btns->addWidget(m_stopButton);
    btns->addStretch();
    run->addLayout(btns);

    auto *telemetry = new QTabWidget(this);
    right->addWidget(telemetry,1);
    auto *metrics = Surface::card(this,"Live metrics");
    m_metrics = new QLabel("Waiting for a training job.",this);
    m_metrics->setObjectName("monoLabel");
    m_metrics->setWordWrap(true);
    metrics->addWidget(m_metrics);
    metrics->addStretch();
    telemetry->addTab(metrics->parentWidget(),"Metrics");
    auto *logs = Surface::card(this,"Training log");
    telemetry->addTab(logs->parentWidget(),"Event log");
    telemetry->setCurrentIndex(0);
    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setPlaceholderText("Training log will appear here.");
    m_log->setMaximumBlockCount(2000);
    m_log->setObjectName("logView");
    m_log->setMinimumHeight(140);
    logs->addWidget(m_log, 1);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName("statusLabel");
    m_statusLabel->setTextFormat(Qt::PlainText);
    m_statusLabel->setWordWrap(true);
    contentLayout()->addWidget(m_statusLabel);
    m_statusLabel->setText("Ready. Select a base model and an ingested dataset.");

    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(kPollMs);
    connect(m_pollTimer, &QTimer::timeout, this, &TrainingPanel::onPollStatus);

    connect(m_startButton, &QPushButton::clicked, this, &TrainingPanel::onStart);
    connect(m_stopButton, &QPushButton::clicked, this, &TrainingPanel::onStop);
}

void TrainingPanel::setupHyperparameters(QWidget *parent) {
    auto *hyperBox = new QGroupBox("Hyperparameters", parent);
    auto *form = new QFormLayout(hyperBox);
    form->setRowWrapPolicy(QFormLayout::WrapAllRows);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

    m_targetModules = new ComboBox(hyperBox);
    m_targetModules->setMinimumContentsLength(10);
    m_targetModules->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_targetModules->addItems({"q_proj,v_proj", "q_proj,k_proj,v_proj,o_proj", "all-linear"});
    form->addRow("Target modules:", m_targetModules);

    m_rankSpin = new QSpinBox(hyperBox);
    m_rankSpin->setRange(1, 512);
    m_rankSpin->setValue(8);
    form->addRow("LoRA rank (r):", m_rankSpin);

    m_alphaSpin = new QSpinBox(hyperBox);
    m_alphaSpin->setRange(1, 1024);
    m_alphaSpin->setValue(16);
    form->addRow("LoRA alpha:", m_alphaSpin);

    m_quantCombo = new ComboBox(hyperBox);
    m_quantCombo->addItems({"None", "4-bit (NF4)", "8-bit (bitsandbytes)"});
    form->addRow("Quantization:", m_quantCombo);

    m_epochsSpin = new QSpinBox(hyperBox);
    m_epochsSpin->setRange(1, 100);
    m_epochsSpin->setValue(3);
    form->addRow("Epochs:", m_epochsSpin);

    m_lrSpin = new QDoubleSpinBox(hyperBox);
    m_lrSpin->setRange(0.0, 1.0);
    m_lrSpin->setDecimals(5);
    m_lrSpin->setSingleStep(0.00001);
    m_lrSpin->setValue(0.0002);
    form->addRow("Learning rate:", m_lrSpin);

    m_batchSpin = new QSpinBox(hyperBox);
    m_batchSpin->setRange(1, 128);
    m_batchSpin->setValue(1);
    form->addRow("Batch size:", m_batchSpin);

    m_warmupSpin = new QDoubleSpinBox(hyperBox);
    m_warmupSpin->setRange(0.0, 1.0);
    m_warmupSpin->setDecimals(2);
    m_warmupSpin->setSingleStep(0.01);
    m_warmupSpin->setValue(0.03);
    form->addRow("Warmup ratio:", m_warmupSpin);

    m_maxLengthSpin = new QSpinBox(hyperBox);
    m_maxLengthSpin->setRange(8, 8192);
    m_maxLengthSpin->setValue(512);
    form->addRow("Max length:", m_maxLengthSpin);

    m_hyperBox = hyperBox;
}

void TrainingPanel::setBackendClient(BackendClient *client) {
    m_client = client;
    if (client && client->isReady()) {
        setStatus("Loading models and datasets...");
        refreshOptions();
    } else {
        setStatus("Backend not connected.");
    }
}

void TrainingPanel::refreshOptions() {
    refreshModels();
    refreshDatasets();
}

void TrainingPanel::refreshModels() {
    if (!m_client || !m_client->isReady()) return;
    const QString prev = m_modelCombo->currentText();
    m_client->get("/models", [this, prev](int status, const QJsonDocument &doc) {
        if (status != 200) return;
        m_modelCombo->blockSignals(true);
        m_modelCombo->clear();
        for (const auto &v : doc.array()) {
            const QJsonObject m = v.toObject();
            m_modelCombo->addItem(m.value("name").toString(), m.value("id").toString());
        }
        m_modelCombo->blockSignals(false);
        applyPreservedText(m_modelCombo, prev);
    });
}

void TrainingPanel::refreshDatasets(const QString &selectId) {
    if (!m_client || !m_client->isReady()) return;
    const QString prev = m_datasetCombo->currentText();
    m_client->get("/datasets", [this, prev, selectId](int status, const QJsonDocument &doc) {
        if (status != 200) return;
        m_datasetCombo->blockSignals(true);
        m_datasetCombo->clear();
        for (const auto &v : doc.array()) {
            const QJsonObject m = v.toObject();
            m_datasetCombo->addItem(m.value("name").toString(), m.value("id").toString());
        }
        m_datasetCombo->blockSignals(false);
        if (!selectId.isEmpty()) {
            const int ix = m_datasetCombo->findData(selectId);
            if (ix >= 0) m_datasetCombo->setCurrentIndex(ix);
        } else {
            applyPreservedText(m_datasetCombo, prev);
        }
    });
}

void TrainingPanel::onQuantizationToggled() {
    if (m_qloraCheck->isChecked()) {
        m_quantCombo->setCurrentIndex(m_quantCombo->findText("4-bit (NF4)"));
        m_quantCombo->setEnabled(false);
    } else {
        m_quantCombo->setCurrentIndex(m_quantCombo->findText("None"));
        m_quantCombo->setEnabled(true);
    }
}

void TrainingPanel::onBrowseBaseModel() {
    const QString path = QFileDialog::getExistingDirectory(
        this, "Select base model directory");
    if (!path.isEmpty()) {
        m_modelCombo->setCurrentText(path);
    }
}

void TrainingPanel::onBrowseDataset() {
    const QString path = QFileDialog::getOpenFileName(
        this, "Import dataset", QString(),
        "Datasets (*.jsonl *.json *.csv *.txt);;All files (*)");
    if (path.isEmpty() || !m_client || !m_client->isReady()) return;

    QJsonObject body;
    body["path"] = path;
    setStatus("Importing dataset...");
    m_client->post("/datasets/ingest", body,
        [this](int status, const QJsonDocument &doc) {
            if (status != 201) {
                if (status == 400) {
                    setStatus("Import failed: " + doc.object()["detail"].toString(), "error");
                } else {
                    setStatus(QString("Import failed (HTTP %1).").arg(status), "error");
                }
                return;
            }
            const QJsonObject rec = doc.object();
            setStatus(QString("Imported dataset \"%1\" (%2 row(s)).")
                          .arg(rec["name"].toString())
                          .arg(rec["rows"].toInt()));
            refreshDatasets(rec["id"].toString());
        });
}

QString TrainingPanel::quantizationCode() const {
    const QString t = m_quantCombo->currentText();
    if (t.startsWith("4-bit")) return "4bit";
    if (t.startsWith("8-bit")) return "8bit";
    return "none";
}

void TrainingPanel::onStart() {
    if (!m_startButton->isEnabled()) return;
    if (!m_client || !m_client->isReady()) {
        setStatus("Backend not connected.", "error");
        return;
    }
    const QString baseText = m_modelCombo->currentText().trimmed();
    if (baseText.isEmpty()) {
        setStatus("Select or type a base model.", "error");
        return;
    }
    const QString dsText = m_datasetCombo->currentText().trimmed();
    const int di = m_datasetCombo->findText(dsText);
    if (di < 0) {
        setStatus("Select an ingested dataset.", "error");
        return;
    }

    const int mi = m_modelCombo->findText(baseText);
    QJsonObject body;
    body["base_model"] = mi >= 0 ? m_modelCombo->itemData(mi).toString() : baseText;
    body["dataset_id"] = m_datasetCombo->itemData(di).toString();
    body["name"] = m_adapterNameEdit->text().trimmed();
    body["rank"] = m_rankSpin->value();
    body["alpha"] = m_alphaSpin->value();
    body["target_modules"] = m_targetModules->currentText();
    body["quantization"] = quantizationCode();
    body["epochs"] = m_epochsSpin->value();
    body["max_steps"] = 0;
    body["learning_rate"] = m_lrSpin->value();
    body["batch_size"] = m_batchSpin->value();
    body["warmup_ratio"] = m_warmupSpin->value();
    body["max_length"] = m_maxLengthSpin->value();

    m_startButton->setEnabled(false);
    m_setup->setEnabled(false);
    m_progress->setValue(0);
    m_log->clear();
    m_activity->setActivity(StudioCanvas::Training);
    m_activity->setMotionEnabled(!QSettings().value("appearance/studio3ReducedMotion",false).toBool());
    m_progressLabel->setText("Preparing your run…");
    m_startButton->setText("Preparing…");
    setStatus("Submitting training job...");
    m_client->post("/training/start", body,
        [this](int status, const QJsonDocument &doc) {
            if (status != 202) {
                m_activity->setActivity(StudioCanvas::Idle);
                m_activity->setMotionEnabled(false);
                m_setup->setEnabled(true);
                m_progressLabel->setText("Ready to retry");
                m_startButton->setText("Start Training");
                m_startButton->setEnabled(true);
                const QString msg = doc.object().value("detail").isString()
                    ? doc.object()["detail"].toString()
                    : QString("HTTP %1").arg(status);
                setStatus("Could not start training: " + msg, "error");
                return;
            }
            m_startButton->setText("Training…");
            m_jobId = doc.object()["id"].toString();
            m_lastLoggedStep = -1;
            m_lastMessage.clear();
            m_progress->setValue(0);
            m_progressLabel->setText("Queued");
            m_metrics->setText("Waiting for the first training step.");
            m_log->appendPlainText(QString("Training job %1 queued.").arg(m_jobId));
            m_stopButton->setEnabled(true);
            m_pollTimer->start();
            onPollStatus();
        });
}

void TrainingPanel::onStop() {
    if (m_jobId.isEmpty() || !m_client || !m_client->isReady()) return;
    if (!m_stopButton->isEnabled()) return;
    m_stopButton->setEnabled(false);
    m_log->appendPlainText("Cancelling training job...");
    m_client->post("/training/cancel/" + m_jobId, QJsonObject(),
        [this](int status, const QJsonDocument &) {
            if (status == 200) {
                setStatus("Cancelling...");
            } else {
                m_stopButton->setEnabled(true);
                setStatus(QString("Cancel request failed (HTTP %1).").arg(status), "error");
            }
        });
}

void TrainingPanel::onPollStatus() {
    if (m_jobId.isEmpty() || !m_client || !m_client->isReady()) {
        m_pollTimer->stop();
        finishJob("Connection lost. Check the local service before starting another run.","error");
        return;
    }
    m_client->get("/training/status/" + m_jobId,
        [this](int status, const QJsonDocument &doc) {
            if (status != 200) {
                m_pollTimer->stop();
                finishJob(QString("Lost track of training job (HTTP %1).").arg(status), "error");
                return;
            }
            const QJsonObject job = doc.object();
            const QString st = job["status"].toString();
            const QString message = job["message"].toString();
            const int step = job["current_step"].toInt();
            const int total = job["total_steps"].toInt();
            const double loss = job["loss"].toDouble(-1.0);
            const int progress = qBound(0,qRound(job["progress"].toDouble()),100);
            m_progress->setValue(progress);
            m_progressLabel->setText(QString("%1% · %2").arg(progress).arg(st));
            m_metrics->setText(QString("Step %1 / %2\nLoss: %3")
                .arg(step).arg(total).arg(loss < 0 ? QString("—") : QString::number(loss,'f',4)));

            if (!message.isEmpty() && message != m_lastMessage) {
                m_lastMessage = message;
                m_log->appendPlainText(message);
            }

            QString tail;
            if (total > 0)
                tail = QString("step %1/%2").arg(step).arg(total);
            else if (step > 0)
                tail = QString("step %1").arg(step);
            if (loss >= 0.0)
                tail += QString("  loss=%1").arg(loss, 0, 'f', 4);

            if (st == "done" || st == "error" || st == "cancelled") {
                m_pollTimer->stop();
                m_log->appendPlainText(QString("[%1] %2").arg(st.toUpper()).arg(message));
                finishJob(message, st == "done" ? "ok" : st == "cancelled" ? "warn" : "error");
                return;
            }

            setStatus(QString("%1%  %2%3")
                          .arg(job["progress"].toDouble(), 0, 'f', 0)
                          .arg(st == "running" ? "" : st + " ")
                          .arg(tail));
        });
}

void TrainingPanel::finishJob(const QString &message, const QString &tone) {
    m_activity->setActivity(StudioCanvas::Idle);
    m_activity->setMotionEnabled(false);
    m_setup->setEnabled(true);
    m_progressLabel->setText(tone == "ok" ? "Training complete" : tone == "warn" ? "Training stopped" : "Training interrupted");
    m_startButton->setText("Start Training");
    m_jobId.clear();
    m_startButton->setEnabled(true);
    m_stopButton->setEnabled(false);
    setStatus(message, tone);
    if (tone == "ok")
        emit trainingCompleted();
}

void TrainingPanel::setStatus(const QString &text, const QString &tone) {
    m_statusLabel->setText(text);
    if (m_runHint) m_runHint->setText(text);
    applyStatusTone(m_statusLabel, tone);
}

QString TrainingPanel::settingsJson() const {
    QJsonObject obj;
    obj["baseModel"] = m_modelCombo->currentText();
    obj["dataset"] = m_datasetCombo->currentText();
    obj["adapterName"] = m_adapterNameEdit->text();
    obj["targetModules"] = m_targetModules->currentText();
    obj["rank"] = m_rankSpin->value();
    obj["alpha"] = m_alphaSpin->value();
    obj["quantization"] = m_quantCombo->currentText();
    obj["epochs"] = m_epochsSpin->value();
    obj["learningRate"] = m_lrSpin->value();
    obj["batchSize"] = m_batchSpin->value();
    obj["warmup"] = m_warmupSpin->value();
    obj["maxLength"] = m_maxLengthSpin->value();
    obj["qlora"] = m_qloraCheck->isChecked();
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void TrainingPanel::applySettingsJson(const QString &json) {
    QJsonParseError err;
    auto doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return;
    auto o = doc.object();
    if (o.contains("baseModel")) m_modelCombo->setCurrentText(o["baseModel"].toString());
    if (o.contains("dataset")) m_datasetCombo->setCurrentText(o["dataset"].toString());
    if (o.contains("adapterName")) m_adapterNameEdit->setText(o["adapterName"].toString());
    if (o.contains("targetModules")) m_targetModules->setCurrentText(o["targetModules"].toString());
    if (o.contains("rank")) m_rankSpin->setValue(o["rank"].toInt());
    if (o.contains("alpha")) m_alphaSpin->setValue(o["alpha"].toInt());
    if (o.contains("epochs")) m_epochsSpin->setValue(o["epochs"].toInt());
    if (o.contains("learningRate")) m_lrSpin->setValue(o["learningRate"].toDouble());
    if (o.contains("batchSize")) m_batchSpin->setValue(o["batchSize"].toInt());
    if (o.contains("warmup")) m_warmupSpin->setValue(o["warmup"].toDouble());
    if (o.contains("maxLength")) m_maxLengthSpin->setValue(o["maxLength"].toInt());
    if (o.contains("qlora")) m_qloraCheck->setChecked(o["qlora"].toBool());
    if (o.contains("quantization")) m_quantCombo->setCurrentText(o["quantization"].toString());
}