#include <QSplitter>
#include <QGridLayout>
#include <QListWidget>
#include <QDateTime>
#include <QFile>
#include <QSaveFile>
#include "widgets/trainingchart.h"
#include <cmath>
#include <QTabWidget>
#include "panels/trainingpanel.h"

#include "backend_client.h"
#include "widgets/surface.h"
#include "widgets/studiocanvas.h"
#include "theme.h"
#include <QProgressBar>
#include <QSettings>
#include <QScrollArea>

#include "widgets/toggleswitch.h"
#include "widgets/combobox.h"
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
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
    columns->setHandleWidth(6);
    contentLayout()->addWidget(columns,1);
    auto *setup = new QWidget(this);
    auto *left = new QVBoxLayout(setup);
    left->setContentsMargins(0,0,0,0);
    left->setSpacing(12);
    auto *setupScroll = new QScrollArea(this);
    setupScroll->setFrameShape(QFrame::NoFrame);
    setupScroll->setWidgetResizable(true);
    setupScroll->setMinimumWidth(290);
    setupScroll->setWidget(setup);
    columns->addWidget(setupScroll);
    m_setup = setupScroll;
    auto *monitor = new QWidget(this);
    auto *right = new QVBoxLayout(monitor);
    right->setContentsMargins(0,0,0,0);
    right->setSpacing(12);
    monitor->setMinimumWidth(310);
    columns->addWidget(monitor);
    columns->setSizes({420,480});

    auto *model = Surface::card(setup,"1. Base model");
    left->addWidget(model->parentWidget());
    m_modelCombo = new ComboBox(this);
    m_modelCombo->setEditable(true);
    m_modelCombo->setMinimumContentsLength(10);
    m_modelCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_modelCombo->setInsertPolicy(QComboBox::NoInsert);
    m_modelCombo->lineEdit()->setPlaceholderText("Choose a model or enter its ID");
    m_modelCombo->setAccessibleName("Base model");
    model->addWidget(m_modelCombo);
    auto *modelRow = new QHBoxLayout();
    auto *refreshBtn = new QPushButton("Refresh", this);
    refreshBtn->setObjectName("secondaryBtn");
    auto *folderBtn = new QPushButton("Folder...", this);
    folderBtn->setObjectName("secondaryBtn");
    modelRow->addWidget(refreshBtn);
    modelRow->addWidget(folderBtn);
    modelRow->addStretch();
    model->addLayout(modelRow);
    auto *modelHint = new QLabel("Training needs original Transformers weights; GGUF is for inference.",this);
    modelHint->setObjectName("cardMeta"); modelHint->setWordWrap(true);
    m_modelCombo->setToolTip(modelHint->text());
    delete modelHint;
    connect(refreshBtn, &QPushButton::clicked, this, &TrainingPanel::refreshOptions);
    connect(folderBtn, &QPushButton::clicked, this, &TrainingPanel::onBrowseBaseModel);

    auto *dataset = Surface::card(setup,"2. Dataset");
    left->addWidget(dataset->parentWidget());
    m_datasetCombo = new ComboBox(this);
    m_datasetCombo->setEditable(true);
    m_datasetCombo->setMinimumContentsLength(10);
    m_datasetCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_datasetCombo->setInsertPolicy(QComboBox::NoInsert);
    m_datasetCombo->lineEdit()->setPlaceholderText("Choose an imported dataset");
    m_datasetCombo->setAccessibleName("Dataset");
    dataset->addWidget(m_datasetCombo);
    auto *ingestBtn = new QPushButton("Import file...", this);
    ingestBtn->setObjectName("secondaryBtn");
    dataset->addWidget(ingestBtn);
    connect(ingestBtn, &QPushButton::clicked, this, &TrainingPanel::onBrowseDataset);
    m_datasetSummary = new QLabel("No dataset selected.",this);
    m_datasetSummary->setObjectName("cardMeta"); m_datasetSummary->setWordWrap(true);
    dataset->addWidget(m_datasetSummary);
    m_validationSpin = new QDoubleSpinBox(this);
    m_validationSpin->setRange(0,50); m_validationSpin->setDecimals(0);
    m_validationSpin->setSingleStep(5); m_validationSpin->setSuffix("%");
    m_validationSpin->setToolTip("Hold out this fraction of rows and measure validation loss after each epoch. 0% disables validation.");
    Surface::field(dataset,"Validation split",m_validationSpin);

    setupHyperparameters(setup);
    left->addWidget(m_hyperBox);
    auto *outputOptions = new QPushButton("Output options ▾",this);
    outputOptions->setCheckable(true); outputOptions->setObjectName("secondaryBtn");
    auto *output = new QWidget(this);
    auto *outputLayout = new QVBoxLayout(output);
    outputLayout->setContentsMargins(0,0,0,0);
    Surface::field(outputLayout,"Adapter name",m_adapterNameEdit=new QLineEdit(this));
    m_adapterNameEdit->setPlaceholderText("Auto-named when empty");
    auto *settingsLayout=qobject_cast<QVBoxLayout *>(m_hyperBox->layout());
    settingsLayout->addWidget(outputOptions); settingsLayout->addWidget(output);
    output->hide();
    connect(outputOptions,&QPushButton::toggled,output,&QWidget::setVisible);
    left->addStretch();

    auto *resources = Surface::card(monitor,"Resource estimate");
    right->addWidget(resources->parentWidget());
    m_inputSummary = new QLabel(this);
    m_inputSummary->setObjectName("cardMeta"); m_inputSummary->setWordWrap(true);
    m_inputSummary->setTextFormat(Qt::PlainText);
    resources->addWidget(m_inputSummary);
    m_estimateLabel=new QLabel("Select a model and dataset to estimate resources.",this);
    m_estimateLabel->setObjectName("cardMeta"); m_estimateLabel->setWordWrap(true);
    m_estimateLabel->setTextFormat(Qt::PlainText); resources->addWidget(m_estimateLabel);
    m_memoryBar=new QProgressBar(this); m_memoryBar->setRange(0,100);
    m_memoryBar->setFixedHeight(6); m_memoryBar->setTextVisible(false);
    resources->addWidget(m_memoryBar); m_memoryBar->hide();
    m_estimateTimer=new QTimer(this); m_estimateTimer->setSingleShot(true); m_estimateTimer->setInterval(300);
    connect(m_estimateTimer,&QTimer::timeout,this,&TrainingPanel::requestEstimate);
    m_startButton = new QPushButton("Start Training", this);
    m_startButton->setObjectName("primaryBtn");
    m_startButton->setIcon(Theme::coloredIcon("play", QColor(Theme::accentText())));

    auto *presets = new QHBoxLayout();
    auto *savePreset = new QPushButton("Save preset…",this);
    auto *loadPreset = m_loadPreset = new QPushButton("Load preset…",this);
    presets->addWidget(m_startButton,1); presets->addWidget(savePreset);
    outputLayout->addWidget(loadPreset);
    resources->addLayout(presets);
    connect(savePreset,&QPushButton::clicked,this,[this] {
        const auto path=QFileDialog::getSaveFileName(this,"Save training preset",QString(),"JSON (*.json)");
        if (path.isEmpty()) return;
        QSaveFile file(path);
        const auto data=settingsJson().toUtf8();
        if (!file.open(QIODevice::WriteOnly) || file.write(data)!=data.size() || !file.commit())
            setStatus("Could not save the training preset.","error");
        else setStatus("Training preset saved.","ok");
    });
    connect(loadPreset,&QPushButton::clicked,this,[this] {
        if (!m_jobId.isEmpty() || !m_startButton->isEnabled()) return;
        const auto path=QFileDialog::getOpenFileName(this,"Load training preset",QString(),"JSON (*.json)");
        if (path.isEmpty()) return;
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly) || file.size()>1024*1024) {
            setStatus("Could not read the training preset.","error"); return;
        }
        const auto data=file.readAll();
        if (!QJsonDocument::fromJson(data).isObject()) {
            setStatus("Training preset must be a JSON object.","error"); return;
        }
        applySettingsJson(QString::fromUtf8(data));
        setStatus("Training preset loaded.");
    });


    auto *run = Surface::card(monitor,"Active run");
    right->addWidget(run->parentWidget());
    auto *session = new QHBoxLayout();
    m_activity = new StudioCanvas(this);
    m_activity->setProperty("brandMark", true);
    m_activity->setProperty("activityIndicator",true);
    m_activity->setObjectName("trainingActivity");
    m_activity->setFixedSize(32,32);
    m_activity->setMotionEnabled(false);
    session->addWidget(m_activity);
    m_runName = new QLabel("No run selected",this);
    m_runName->setTextFormat(Qt::PlainText); m_runName->setWordWrap(true);
    session->addWidget(m_runName,1);
    run->addLayout(session);
    m_progressLabel = new QLabel("Ready to train",this);
    m_progressLabel->setObjectName("metricValue");
    m_progressLabel->setWordWrap(true);

    m_visualise=new ToggleSwitch("Visualisation",this); m_visualise->setChecked(true);
    m_visualise->setToolTip("Switch off to inspect the raw job data.");
    auto *viewRow=new QHBoxLayout();
    viewRow->addWidget(m_progressLabel,1); viewRow->addWidget(m_visualise); run->addLayout(viewRow);
    auto *charts = new QTabWidget(this);
    m_chartTabs=charts;
    charts->setObjectName("trainingCharts");
    m_lossChart = new TrainingChart("loss",this);
    m_lossChart->setObjectName("trainingLossChart");
    m_lrChart = new TrainingChart("learning_rate",this);
    m_gradChart = new TrainingChart("grad_norm",this);
    charts->addTab(m_lossChart,"Loss");
    charts->addTab(m_lrChart,"Learning rate");
    charts->addTab(m_gradChart,"Gradient norm");
    run->addWidget(charts);
    m_rawData=new QPlainTextEdit(this); m_rawData->setReadOnly(true);
    m_rawData->setObjectName("trainingRawData"); m_rawData->setMinimumHeight(180);
    m_rawData->setMaximumHeight(260); run->addWidget(m_rawData); m_rawData->hide();
    connect(m_visualise,&QCheckBox::toggled,this,[this](bool enabled) {
        m_chartTabs->setVisible(enabled); m_rawData->setVisible(!enabled);
    });
    auto *legend = new QLabel("Solid: training  ·  Dashed: validation (when enabled)",this);
    legend->setObjectName("cardMeta"); legend->setWordWrap(true);
    connect(m_visualise,&QCheckBox::toggled,legend,&QWidget::setVisible);
    run->addWidget(legend);
    m_metrics = new QLabel("Waiting for a training job.",this);
    m_metrics->setObjectName("cardMeta"); m_metrics->setWordWrap(true);
    m_metrics->setTextFormat(Qt::PlainText);
    run->addWidget(m_metrics);
    m_progress = new QProgressBar(this);
    m_progress->setRange(0,100); m_progress->setValue(0);
    m_progress->setTextVisible(false); m_progress->setFixedHeight(8);
    run->addWidget(m_progress);
    m_runHint = new QLabel("Choose a model and dataset, then start your run.",this);
    m_runHint->setObjectName("cardMeta"); m_runHint->setWordWrap(true);
    m_runHint->setTextFormat(Qt::PlainText);
    m_runHint->hide(); // Full status is below the workspace; keep the chart visible.
    m_stopButton = new QPushButton("Stop", this);
    m_stopButton->setObjectName("destructiveBtn");
    m_stopButton->setEnabled(false);
    run->addWidget(m_stopButton,0,Qt::AlignLeft);

    auto *history = Surface::card(monitor,"Past runs");
    right->addWidget(history->parentWidget());
    m_runs = new QListWidget(this);
    m_runs->setObjectName("trainingRuns"); m_runs->setAccessibleName("Past training runs");
    m_runs->setMinimumHeight(100); m_runs->setMaximumHeight(175);
    m_runs->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    connect(m_runs,&QListWidget::itemClicked,this,[this](QListWidgetItem *item) { emit runDetailsRequested(item->data(Qt::UserRole).toString()); });
    connect(m_runs,&QListWidget::itemActivated,this,[this](QListWidgetItem *item) { emit runDetailsRequested(item->data(Qt::UserRole).toString()); });
    history->addWidget(m_runs);
    connect(m_runs,&QListWidget::currentRowChanged,this,[this](int row) {
        if (!m_jobId.isEmpty() || !m_startButton->isEnabled() || row<0 || row>=m_jobs.size()) return;
        const auto job=m_jobs[row].toObject();
        displayJob(job);
        m_progressLabel->setText(job["status"].toString()=="done" ? "Training complete" : job["status"].toString());
        m_runHint->setText(job["message"].toString());
        m_log->setPlainText(job["message"].toString());
    });
    auto *events = new QPushButton("Event log ▾",this);
    events->setCheckable(true);
    right->addWidget(events);
    m_log = new QPlainTextEdit(this);
    m_log->setReadOnly(true); m_log->setMaximumBlockCount(2000);
    m_log->setObjectName("logView"); m_log->setMinimumHeight(140);
    right->addWidget(m_log); m_log->hide();
    connect(events,&QPushButton::toggled,m_log,&QWidget::setVisible);
    right->addStretch();

    m_statusLabel = new QLabel("Ready. Select a base model and an imported dataset.",this);
    m_statusLabel->setObjectName("statusLabel");
    m_statusLabel->setTextFormat(Qt::PlainText); m_statusLabel->setWordWrap(true);
    contentLayout()->addWidget(m_statusLabel);
    m_pollTimer = new QTimer(this); m_pollTimer->setInterval(kPollMs);
    connect(m_pollTimer,&QTimer::timeout,this,&TrainingPanel::onPollStatus);
    connect(m_startButton,&QPushButton::clicked,this,&TrainingPanel::onStart);
    connect(m_stopButton,&QPushButton::clicked,this,&TrainingPanel::onStop);
    connect(m_modelCombo,&QComboBox::currentTextChanged,this,&TrainingPanel::updateInputSummary);
    connect(m_datasetCombo,&QComboBox::currentTextChanged,this,&TrainingPanel::updateInputSummary);
    connect(m_batchSpin,&QSpinBox::valueChanged,this,&TrainingPanel::updateInputSummary);
    connect(m_epochsSpin,&QSpinBox::valueChanged,this,&TrainingPanel::updateInputSummary);
    connect(m_validationSpin,&QDoubleSpinBox::valueChanged,this,&TrainingPanel::updateInputSummary);
    for (auto *spin : {m_rankSpin,m_alphaSpin,m_maxLengthSpin})
        connect(spin,&QSpinBox::valueChanged,this,&TrainingPanel::updateInputSummary);
    connect(m_quantCombo,&QComboBox::currentIndexChanged,this,&TrainingPanel::updateInputSummary);
    connect(m_targetModules,&QComboBox::currentIndexChanged,this,&TrainingPanel::updateInputSummary);
    updateInputSummary();
}

void TrainingPanel::setupHyperparameters(QWidget *parent) {
    auto *card=Surface::card(parent,"3. Settings");
    m_hyperBox=card->parentWidget();
    auto *grid = new QGridLayout();
    card->addLayout(grid);
    grid->setHorizontalSpacing(8);
    auto field = [grid](int row,int col,const QString &title,QWidget *editor) {
        auto *label=new QLabel(title,editor->parentWidget()); label->setObjectName("cardMeta");
        label->setBuddy(editor); editor->setAccessibleName(title);
        grid->addWidget(label,row*2,col); grid->addWidget(editor,row*2+1,col);
        grid->setColumnStretch(col,1);
        editor->setMinimumWidth(0);
        editor->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Fixed);
    };
    m_rankSpin=new QSpinBox(m_hyperBox); m_rankSpin->setRange(1,512); m_rankSpin->setValue(8);
    m_alphaSpin=new QSpinBox(m_hyperBox); m_alphaSpin->setRange(1,1024); m_alphaSpin->setValue(16);
    m_dropoutSpin=new QDoubleSpinBox(m_hyperBox); m_dropoutSpin->setRange(0,0.99);
    m_dropoutSpin->setDecimals(2); m_dropoutSpin->setSingleStep(0.01); m_dropoutSpin->setValue(0.05);
    field(0,0,"Rank",m_rankSpin); field(0,1,"Alpha",m_alphaSpin); field(0,2,"Dropout",m_dropoutSpin);
    m_epochsSpin=new QSpinBox(m_hyperBox); m_epochsSpin->setRange(1,100); m_epochsSpin->setValue(3);
    m_lrSpin=new QDoubleSpinBox(m_hyperBox); m_lrSpin->setRange(0.000001,1);
    m_lrSpin->setDecimals(6); m_lrSpin->setSingleStep(0.00001); m_lrSpin->setValue(0.0002);
    m_batchSpin=new QSpinBox(m_hyperBox); m_batchSpin->setRange(1,128); m_batchSpin->setValue(1);
    field(1,0,"Epochs",m_epochsSpin); field(1,1,"Learning rate",m_lrSpin); field(1,2,"Batch size",m_batchSpin);
    m_targetModules=new ComboBox(m_hyperBox);
    m_targetModules->setMinimumContentsLength(5);
    m_targetModules->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_targetModules->addItems({"q_proj,v_proj","q_proj,k_proj,v_proj,o_proj","all-linear"});
    m_maxLengthSpin=new QSpinBox(m_hyperBox); m_maxLengthSpin->setRange(8,8192); m_maxLengthSpin->setValue(512);
    m_warmupSpin=new QDoubleSpinBox(m_hyperBox); m_warmupSpin->setRange(0,1);
    m_warmupSpin->setDecimals(2); m_warmupSpin->setSingleStep(0.01); m_warmupSpin->setValue(0.03);
    field(2,0,"Target modules",m_targetModules); field(2,1,"Max length",m_maxLengthSpin); field(2,2,"Warmup ratio",m_warmupSpin);
    m_quantCombo=new ComboBox(m_hyperBox);
    m_quantCombo->addItems({"None","4-bit (NF4)","8-bit (bitsandbytes)"});
    m_quantCombo->setAccessibleName("Quantization");
    grid->addWidget(m_quantCombo,6,0,1,3);
    m_qloraCheck=new ToggleSwitch("Use 4-bit QLoRA",m_hyperBox);
    m_qloraCheck->setChecked(true);
    grid->addWidget(m_qloraCheck,7,0,1,3);
    connect(m_qloraCheck,&QCheckBox::toggled,this,&TrainingPanel::onQuantizationToggled);
    onQuantizationToggled();
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
    refreshCapabilities();
    refreshJobs();
}

void TrainingPanel::refreshCapabilities() {
    if (!m_client || !m_client->isReady()) return;
    m_client->get("/training/capabilities", [this](int status, const QJsonDocument &doc) {
        if (status != 200 || !doc.isObject()) return;   // older backend: keep every option
        m_quantizationAvailable = doc.object()["quantization"].toBool(true);
        m_quantizationDetail = doc.object()["quantization_detail"].toString();
        applyQuantizationAvailability();
    });
}

// Also overrides a saved 4-bit choice, so existing settings cannot start a job
// the backend would refuse.
void TrainingPanel::applyQuantizationAvailability() {
    if (m_quantizationAvailable) {
        m_qloraCheck->setEnabled(true);
        m_qloraCheck->setToolTip("Uses 4-bit NF4 weights. Turn off to choose another precision in Hyperparameters.");
        m_quantCombo->setToolTip(QString());
        return;
    }
    m_qloraCheck->setChecked(false);
    m_qloraCheck->setEnabled(false);
    m_qloraCheck->setToolTip(m_quantizationDetail);
    m_quantCombo->setCurrentIndex(m_quantCombo->findText("None"));
    m_quantCombo->setEnabled(false);
    m_quantCombo->setToolTip(m_quantizationDetail);
}

void TrainingPanel::refreshModels() {
    if (!m_client || !m_client->isReady()) return;
    const QString prev = m_modelCombo->currentText();
    m_client->get("/models", [this, prev](int status, const QJsonDocument &doc) {
        if (status != 200) return;
        m_models = doc.array();
        m_modelCombo->blockSignals(true);
        m_modelCombo->clear();
        for (const auto &v : doc.array()) {
            const QJsonObject m = v.toObject();
            m_modelCombo->addItem(m.value("name").toString(), m.value("id").toString());
        }
        m_modelCombo->blockSignals(false);
        applyPreservedText(m_modelCombo, prev);
        updateInputSummary();
    });
}

void TrainingPanel::refreshDatasets(const QString &selectId) {
    if (!m_client || !m_client->isReady()) return;
    const QString prev = m_datasetCombo->currentText();
    m_client->get("/datasets", [this, prev, selectId](int status, const QJsonDocument &doc) {
        if (status != 200) return;
        m_datasets = doc.array();
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
        updateInputSummary();
    });
}

void TrainingPanel::onQuantizationToggled() {
    if (m_qloraCheck->isChecked()) {
        m_quantCombo->setCurrentIndex(m_quantCombo->findText("4-bit (NF4)"));
        m_quantCombo->setEnabled(false);
    } else {
        m_quantCombo->setCurrentIndex(m_quantCombo->findText("None"));
        m_quantCombo->setEnabled(m_quantizationAvailable);
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
    body["dropout"] = m_dropoutSpin->value();
    body["validation_split"] = m_validationSpin->value()/100.0;
    body["target_modules"] = m_targetModules->currentText();
    body["quantization"] = quantizationCode();
    body["epochs"] = m_epochsSpin->value();
    body["max_steps"] = 0;
    body["learning_rate"] = m_lrSpin->value();
    body["batch_size"] = m_batchSpin->value();
    body["warmup_ratio"] = m_warmupSpin->value();
    body["max_length"] = m_maxLengthSpin->value();

    ++m_jobsRequest; // Ignore list replies issued before this submission.
    m_startButton->setEnabled(false);
    m_loadPreset->setEnabled(false);
    m_setup->setEnabled(false);
    m_progress->setValue(0);
    m_log->clear();
    m_lossChart->setHistory({}); m_lrChart->setHistory({}); m_gradChart->setHistory({});
    m_metrics->setText("Waiting for measured samples.");
    m_runName->setText(m_adapterNameEdit->text().trimmed().isEmpty() ? "New training run" : m_adapterNameEdit->text().trimmed());
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
                m_runs->setEnabled(true);
                m_loadPreset->setEnabled(true);
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
    const auto id=m_jobId;
    m_client->post("/training/cancel/" + id, QJsonObject(),
        [this,id](int status, const QJsonDocument &) {
            if (m_jobId != id) return;
            if (status == 200) {
                setStatus("Cancelling...");
            } else {
                m_stopButton->setEnabled(true);
                setStatus(QString("Cancel request failed (HTTP %1).").arg(status), "error");
            }
        });
}

void TrainingPanel::onPollStatus() {
    if (m_jobId.isEmpty() || m_pollPending) return;
    if (!m_client || !m_client->isReady()) {
        setStatus("Training status unavailable. Reconnecting; the backend may still be training.","warn");
        return;
    }
    const QString id=m_jobId;
    m_pollPending=true;
    m_client->get("/training/status/" + id,
        [this,id](int status, const QJsonDocument &doc) {
            m_pollPending=false;
            if (m_jobId != id) return;
            if (status != 200 || !doc.isObject()) {
                if (status == 404) {
                    m_pollTimer->stop();
                    finishJob("The backend no longer has this training job.","error");
                } else setStatus(QString("Status request failed (HTTP %1). Retrying; the job may still be running.").arg(status),"warn");
                return;
            }
            const auto job=doc.object();
            displayJob(job);
            const QString st=job["status"].toString();
            const QString message=job["message"].toString();
            if (!message.isEmpty() && message!=m_lastMessage) {
                m_lastMessage=message; m_log->appendPlainText(message);
            }
            if (st=="done" || st=="error" || st=="cancelled") {
                m_pollTimer->stop();
                m_log->appendPlainText(QString("[%1] %2").arg(st.toUpper(),message));
                finishJob(message,st=="done" ? "ok" : st=="cancelled" ? "warn" : "error");
            } else setStatus(message);
        });
}

void TrainingPanel::displayJob(const QJsonObject &job) {
    m_rawData->setPlainText(QString::fromUtf8(QJsonDocument(job).toJson(QJsonDocument::Indented)));
    const auto history=job["history"].toArray();
    const int total=job["total_steps"].toInt();
    const int step=job["current_step"].toInt();
    m_lossChart->setHistory(history,total);
    m_lrChart->setHistory(history,total);
    m_gradChart->setHistory(history,total);
    m_runName->setText(job["name"].toString("Training run"));
    const double progress=job["progress"].toDouble();
    m_progress->setValue(std::isfinite(progress) ? qBound(0,qRound(progress),100) : 0);
    m_progressLabel->setText(QString("%1% · %2").arg(m_progress->value()).arg(job["status"].toString()));
    auto number=[](QJsonValue value) {
        return value.isDouble() && std::isfinite(value.toDouble()) ? QString::number(value.toDouble(),'g',5) : QString("—");
    };
    QJsonValue validation, rate, gradient, epoch;
    double elapsed=0;
    for (const auto &v : history) {
        const auto row=v.toObject();
        if (row.contains("eval_loss")) validation=row["eval_loss"];
        if (row.contains("learning_rate")) rate=row["learning_rate"];
        if (row.contains("grad_norm")) gradient=row["grad_norm"];
        if (row.contains("epoch")) epoch=row["epoch"];
        elapsed=row["elapsed_seconds"].toDouble(elapsed);
    }
    QString timing="Time remaining: —";
    // Use differences between measured steps, so model loading and evaluation
    // before the first sample are not treated as optimiser throughput.
    QJsonObject first, last;
    for (const auto &v : history) {
        const auto row=v.toObject();
        if (!row["loss"].isDouble() || !row["elapsed_seconds"].isDouble()) continue;
        if (first.isEmpty()) first=row;
        last=row;
    }
    const double steps=last["step"].toDouble()-first["step"].toDouble();
    const double seconds=last["elapsed_seconds"].toDouble()-first["elapsed_seconds"].toDouble();
    if (steps>0 && seconds>0 && job["status"].toString()=="running" && total>step) {
        const double remaining=seconds/steps*(total-step);
        if (std::isfinite(remaining)) timing=QString("~%1 min left · %2 steps/s")
            .arg(qMax(1.0,std::ceil(remaining/60.0)),0,'f',0).arg(steps/seconds,0,'f',2);
    }
    const auto began=QDateTime::fromString(job["started_at"].toString(),Qt::ISODate);
    const auto ended=QDateTime::fromString(job["finished_at"].toString(),Qt::ISODate);
    if (began.isValid()) elapsed=qMax(qint64(0),began.secsTo(ended.isValid()?ended:QDateTime::currentDateTimeUtc()));
    m_metrics->setText(QString("Train %1  ·  Validation %2\nStep %3 / %4  ·  Epoch %5\nLR %6  ·  Gradient %7\n%8 min elapsed · %9")
        .arg(number(job["loss"]),number(validation)).arg(step).arg(total>0?QString::number(total):"—")
        .arg(number(epoch),number(rate),number(gradient)).arg(elapsed/60.0,0,'f',1).arg(timing));
}

void TrainingPanel::refreshJobs() {
    if (!m_client || !m_client->isReady()) return;
    const int request=++m_jobsRequest;
    m_client->get("/training/jobs",[this,request](int status,const QJsonDocument &doc) {
        if (request!=m_jobsRequest) return;
        if (status!=200 || !doc.isArray()) return;
        const QString selected=m_runs->currentItem() ? m_runs->currentItem()->data(Qt::UserRole).toString() : QString();
        m_jobs=doc.array();
        m_runs->blockSignals(true); m_runs->clear();
        QString active;
        for (const auto &v : m_jobs) {
            const auto job=v.toObject();
            const auto st=job["status"].toString();
            auto *item=new QListWidgetItem(m_runs);
            item->setData(Qt::UserRole,job["id"].toString());
            item->setToolTip("Review "+job["name"].toString());
            auto *row=new QWidget(m_runs); row->setObjectName("trainingRunRow");
            auto *line=new QHBoxLayout(row); line->setContentsMargins(8,7,8,7);
            auto *name=new QLabel(job["name"].toString(),row);
            name->setTextFormat(Qt::PlainText); name->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
            line->addWidget(name,1);
            QString result=st=="done" ? "Done" : st=="error" ? "Failed" : st=="cancelled" ? "Stopped" : "In progress";
            if (st=="done" && job["loss"].isDouble()) result+=QString(" · loss %1").arg(job["loss"].toDouble(),0,'f',2);
            auto *state=new QLabel(result,row); state->setObjectName("runState");
            state->setProperty("state",st=="done" ? "ok" : st=="error" ? "error" : "warn");
            line->addWidget(state); line->addWidget(new QLabel("›",row));
            row->setAttribute(Qt::WA_TransparentForMouseEvents);
            item->setSizeHint(QSize(0,42));
            item->setData(Qt::AccessibleTextRole,name->text()+", "+result+", open run details");
            m_runs->setItemWidget(item,row);
            if (job["id"].toString()==selected) m_runs->setCurrentItem(item);
            if (active.isEmpty() && (st=="running" || st=="queued")) active=job["id"].toString();
        }
        m_runs->blockSignals(false);
        // Reattach to a job still running after switching/reopening the page.
        if (m_jobId.isEmpty() && m_startButton->isEnabled() && !active.isEmpty()) {
            m_jobId=active; m_lastMessage.clear();
            m_setup->setEnabled(false);
            m_startButton->setEnabled(false); m_startButton->setText("Training…");
            m_loadPreset->setEnabled(false);
            m_stopButton->setEnabled(true);
            m_activity->setActivity(StudioCanvas::Training);
            m_activity->setMotionEnabled(!QSettings().value("appearance/studio3ReducedMotion",false).toBool());
            m_pollTimer->start(); onPollStatus();
        }
    });
}

void TrainingPanel::updateInputSummary() {
    QString size="Model size: unavailable";
    const int mi=m_modelCombo->findText(m_modelCombo->currentText());
    if (mi>=0 && mi<m_models.size()) {
        const auto m=m_models[mi].toObject();
        if (m["size_mb"].toDouble()>0) size=QString("Model on disk: %1 MB").arg(m["size_mb"].toDouble(),0,'f',0);
    }
    QString details="Dataset metadata unavailable";
    const int di=m_datasetCombo->findText(m_datasetCombo->currentText());
    if (di>=0 && di<m_datasets.size()) {
        const auto d=m_datasets[di].toObject();
        details=QString("%1 · %2 rows").arg(d["format"].toString("Unknown format")).arg(d["rows"].toInt());
    }
    m_datasetSummary->setText(details);
    m_inputSummary->setText(QString("%1 · %2\n%3 epochs · batch %4 · %5% validation")
        .arg(size,details).arg(m_epochsSpin->value()).arg(m_batchSpin->value()).arg(m_validationSpin->value(),0,'f',0));
    if (m_estimateTimer) { ++m_estimateRequest; m_estimateTimer->start(); }
}

void TrainingPanel::requestEstimate() {
    if (!m_client || !m_client->isReady()) return;
    const int di=m_datasetCombo->findText(m_datasetCombo->currentText());
    if (di<0 || m_modelCombo->currentText().trimmed().isEmpty()) return;
    const int mi=m_modelCombo->findText(m_modelCombo->currentText());
    QJsonObject body{{"base_model",mi<0 ? m_modelCombo->currentText() : m_modelCombo->itemData(mi).toString()},
        {"dataset_id",m_datasetCombo->itemData(di).toString()},{"rank",m_rankSpin->value()},
        {"epochs",m_epochsSpin->value()},{"batch_size",m_batchSpin->value()},
        {"max_length",m_maxLengthSpin->value()},{"target_modules",m_targetModules->currentText()},
        {"quantization",quantizationCode()},{"validation_split",m_validationSpin->value()/100.0}};
    const int request=m_estimateRequest;
    m_client->post("/training/estimate",body,[this,request](int status,const QJsonDocument &doc) {
        if (request!=m_estimateRequest) return;
        m_memoryBar->hide();
        if (status!=200 || !doc.isObject()) { m_estimateLabel->setText("Resource estimate unavailable. Refresh to retry."); return; }
        const auto e=doc.object();
        auto gb=[](double bytes) { return QString::number(bytes/(1024.0*1024*1024),'f',1)+" GB"; };
        const auto memory=e["memory"].toObject();
        const double available=memory["available_bytes"].toDouble();
        const double low=e["memory_low_bytes"].toDouble(), high=e["memory_high_bytes"].toDouble();
        QStringList lines;
        lines << (high>0 ? "Memory needed: ~"+gb(low)+"–"+gb(high) : "Memory needed: unavailable for this model");
        lines << (available>0 ? memory["kind"].toString()+" available: "+gb(available) : memory["kind"].toString()+": available memory unknown");
        if (e["adapter_bytes"].toDouble()>0) lines << QString("Adapter: ~%1 MB").arg(e["adapter_bytes"].toDouble()/(1024*1024),0,'f',1);
        if (e["duration_high_seconds"].toDouble()>0)
            lines << QString("Estimated time: ~%1–%2 min").arg(qMax(1.0,std::ceil(e["duration_low_seconds"].toDouble()/60.0)),0,'f',0).arg(qMax(1.0,std::ceil(e["duration_high_seconds"].toDouble()/60.0)),0,'f',0);
        else lines << "Estimated time: calibrates after a matching run";
        if (e["steps"].isDouble()) lines << QString("%1 optimiser steps").arg(e["steps"].toInt());
        m_estimateLabel->setText(lines.join('\n'));
        m_estimateLabel->setToolTip(e["assumptions"].toString()+"\n"+e["detail"].toString());
        if (available>0 && high>0) { m_memoryBar->setValue(qBound(0,qRound(high/available*100),100)); m_memoryBar->show(); }
    });
}

void TrainingPanel::finishJob(const QString &message, const QString &tone) {
    m_activity->setActivity(StudioCanvas::Idle);
    m_activity->setMotionEnabled(false);
    m_setup->setEnabled(true);
    m_runs->setEnabled(true);
    m_loadPreset->setEnabled(true);
    m_progressLabel->setText(tone == "ok" ? "Training complete" : tone == "warn" ? "Training stopped" : "Training interrupted");
    m_startButton->setText("Start Training");
    m_jobId.clear();
    m_startButton->setEnabled(true);
    m_stopButton->setEnabled(false);
    setStatus(message, tone);
    refreshJobs();
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
    obj["dropout"] = m_dropoutSpin->value();
    obj["validationSplit"] = m_validationSpin->value();
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
    if (o.contains("dropout")) m_dropoutSpin->setValue(o["dropout"].toDouble());
    if (o.contains("validationSplit")) m_validationSpin->setValue(o["validationSplit"].toDouble());
    if (o.contains("epochs")) m_epochsSpin->setValue(o["epochs"].toInt());
    if (o.contains("learningRate")) m_lrSpin->setValue(o["learningRate"].toDouble());
    if (o.contains("batchSize")) m_batchSpin->setValue(o["batchSize"].toInt());
    if (o.contains("warmup")) m_warmupSpin->setValue(o["warmup"].toDouble());
    if (o.contains("maxLength")) m_maxLengthSpin->setValue(o["maxLength"].toInt());
    if (o.contains("qlora")) m_qloraCheck->setChecked(o["qlora"].toBool());
    if (o.contains("quantization")) m_quantCombo->setCurrentText(o["quantization"].toString());
    if (!m_quantizationAvailable) applyQuantizationAvailability();
    updateInputSummary();
}