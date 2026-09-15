#include "panels/trainingpanel.h"

#include "backend_client.h"

#include <QCheckBox>
#include <QComboBox>
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
    , m_cacheCheck(nullptr)
    , m_startButton(nullptr)
    , m_stopButton(nullptr)
    , m_log(nullptr)
    , m_statusLabel(nullptr)
{
    setHeaderSubtitle("Fine-tune a downloaded model locally with LoRA or QLoRA.");
    setupUi();
}

QLabel* TrainingPanel::addField(QWidget *editor, const QString &label) {
    auto *lab = new QLabel(label, this);
    auto *row = new QHBoxLayout();
    row->addWidget(lab);
    row->addWidget(editor, 1);
    contentLayout()->addLayout(row);
    return lab;
}

void TrainingPanel::setupUi() {
    auto *layout = contentLayout();

    m_modelCombo = new QComboBox(this);
    m_modelCombo->setEditable(true);
    m_modelCombo->setInsertPolicy(QComboBox::NoInsert);
    m_modelCombo->setPlaceholderText("e.g. meta-llama/Llama-3.2-1B-Instruct");
    addField(m_modelCombo, "Base model:");
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

    m_datasetCombo = new QComboBox(this);
    m_datasetCombo->setEditable(true);
    m_datasetCombo->setInsertPolicy(QComboBox::NoInsert);
    m_datasetCombo->setPlaceholderText("Select an ingested dataset");
    addField(m_datasetCombo, "Dataset:");
    auto *datasetRow = new QHBoxLayout();
    auto *ingestBtn = new QPushButton("Import file...", this);
    ingestBtn->setObjectName("secondaryBtn");
    datasetRow->addWidget(ingestBtn);
    datasetRow->addStretch();
    layout->addLayout(datasetRow);
    connect(ingestBtn, &QPushButton::clicked, this, &TrainingPanel::onBrowseDataset);

    addField(m_adapterNameEdit = new QLineEdit(this), "Adapter name:");
    m_adapterNameEdit->setPlaceholderText("Auto-named from job if empty");

    setupHyperparameters(this);
    layout->addWidget(m_hyperBox);

    m_qloraCheck = new QCheckBox("Use QLoRA (quantized base model)", this);
    m_qloraCheck->setChecked(true);
    layout->addWidget(m_qloraCheck);

    m_cacheCheck = new QCheckBox("Cache trained adapter (for hot-switching)", this);
    m_cacheCheck->setChecked(true);
    layout->addWidget(m_cacheCheck);

    connect(m_qloraCheck, &QCheckBox::toggled, this, &TrainingPanel::onQuantizationToggled);
    onQuantizationToggled();

    auto *btns = new QHBoxLayout();
    m_startButton = new QPushButton("Start Training", this);
    m_startButton->setObjectName("primaryBtn");
    m_stopButton = new QPushButton("Stop", this);
    m_stopButton->setObjectName("destructiveBtn");
    m_stopButton->setEnabled(false);
    btns->addWidget(m_startButton);
    btns->addWidget(m_stopButton);
    btns->addStretch();
    layout->addLayout(btns);

    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true);
    m_log->setPlaceholderText("Training log will appear here.");
    m_log->setMaximumBlockCount(2000);
    m_log->setObjectName("logView");
    layout->addWidget(m_log, 1);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName("statusLabel");
    layout->addWidget(m_statusLabel);
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

    m_targetModules = new QComboBox(hyperBox);
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

    m_quantCombo = new QComboBox(hyperBox);
    m_quantCombo->addItems({"None", "4-bit (NF4)", "8-bit (FP8)"});
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
    setStatus("Submitting training job...");
    m_client->post("/training/start", body,
        [this](int status, const QJsonDocument &doc) {
            if (status != 202) {
                m_startButton->setEnabled(true);
                const QString msg = doc.object().value("detail").isString()
                    ? doc.object()["detail"].toString()
                    : QString("HTTP %1").arg(status);
                setStatus("Could not start training: " + msg, "error");
                return;
            }
            m_jobId = doc.object()["id"].toString();
            m_lastLoggedStep = -1;
            m_lastMessage.clear();
            m_log->appendPlainText(QString("Training job %1 queued.").arg(m_jobId));
            m_stopButton->setEnabled(true);
            m_pollTimer->start();
            onPollStatus();
        });
}

void TrainingPanel::onStop() {
    if (m_jobId.isEmpty() || !m_client || !m_client->isReady()) return;
    m_log->appendPlainText("Cancelling training job...");
    m_client->post("/training/cancel/" + m_jobId, QJsonObject(),
        [this](int status, const QJsonDocument &) {
            if (status == 200) {
                setStatus("Cancelling...");
            } else {
                setStatus(QString("Cancel request failed (HTTP %1).").arg(status), "error");
            }
        });
}

void TrainingPanel::onPollStatus() {
    if (m_jobId.isEmpty() || !m_client || !m_client->isReady()) {
        m_pollTimer->stop();
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
                finishJob(message, st == "done" ? "ok" : "error");
                return;
            }

            setStatus(QString("%1%  %2%3")
                          .arg(job["progress"].toDouble(), 0, 'f', 0)
                          .arg(st == "running" ? "" : st + " ")
                          .arg(tail));
        });
}

void TrainingPanel::finishJob(const QString &message, const QString &tone) {
    m_jobId.clear();
    m_startButton->setEnabled(true);
    m_stopButton->setEnabled(false);
    setStatus(message, tone);
    if (tone != "error")
        emit trainingCompleted();
}

void TrainingPanel::setStatus(const QString &text, const QString &tone) {
    m_statusLabel->setText(text);
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
}