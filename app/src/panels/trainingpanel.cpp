#include "widgets/spinbox.h"
#include "widgets/tabwidget.h"
#include <QTableWidget>
#include <QStackedWidget>
#include <QResizeEvent>
#include <QBoxLayout>
#include <QDialog>
#include <QStyle>
#include <QMenu>
#include <QScreen>
#include <QGuiApplication>
#include <QAbstractItemView>
#include <QShortcut>
#include <QApplication>
#include <QClipboard>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QSyntaxHighlighter>
#include <QTextCharFormat>
#include <QTextDocument>
#include <QTextCursor>
#include <QHeaderView>
#include <QStandardItemModel>
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
#include "trainingduration.h"
#include <QProgressBar>
#include "widgets/progressbar.h"
#include <QSettings>
#include <QSignalBlocker>
#include <QScrollArea>
#include <QScrollBar>

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
#include "widgets/button.h"
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

namespace {
constexpr int kPollMs = 1000;

class TrainingLogHighlighter : public QSyntaxHighlighter {
public:
    explicit TrainingLogHighlighter(QTextDocument *document) : QSyntaxHighlighter(document) {}
protected:
    void highlightBlock(const QString &text) override {
        QTextCharFormat time; time.setForeground(QColor(Theme::textMuted())); setFormat(0,8,time);
        const auto level=text.mid(10,9);
        QTextCharFormat severity;
        severity.setForeground(QColor(level.contains("ERROR")?Theme::statusError():level.contains("WARN")?Theme::statusWarning():level.contains("SUCCESS")?Theme::statusSuccess():Theme::textSecondary()));
        severity.setFontWeight(QFont::DemiBold); setFormat(10,9,severity);
    }
};


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
    : BasePanel("Training studio", parent, true)
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
    setupUi();
}

void TrainingPanel::setupUi() {
    setObjectName("trainingPage");
    contentLayout()->setSpacing(20);
    auto label=[this](const QString &text,const char *name) {
        auto *value=new QLabel(text,this); value->setObjectName(name); value->setTextFormat(Qt::PlainText);
        if (QString::fromLatin1(name)=="trainingBadge") {
            value->setAlignment(Qt::AlignCenter); value->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Fixed); value->setFixedHeight(24);
        }
        return value;
    };
    auto *navigation=new QHBoxLayout(); navigation->setSpacing(16);
    m_breadcrumb=label("Workspace   /   Training   /   Studio","trainingBreadcrumb");
    navigation->addWidget(m_breadcrumb); navigation->addStretch();
    m_workspaceTabs=new SegmentedTabBar(this); m_workspaceTabs->setObjectName("trainingWorkspaceTabs");
    m_workspaceTabs->addTab("Studio"); m_workspaceTabs->addTab("Observatory"); m_workspaceTabs->setProperty("compact",true); m_workspaceTabs->setFixedHeight(28);
    m_workspaceTabs->setAccessibleName("Training workspace view"); navigation->addWidget(m_workspaceTabs);
    contentLayout()->addLayout(navigation);
    auto *heading=new QHBoxLayout(); heading->setSpacing(16);
    auto *titles=new QVBoxLayout(); titles->setSpacing(7);
    auto *titleRow=new QHBoxLayout(); titleRow->setSpacing(18);
    m_pageTitle=label("Training studio","trainingPageTitle"); titleRow->addWidget(m_pageTitle);
    titleRow->addWidget(label("CREATE / ITERATE","trainingTitleMarker")); titleRow->addStretch();
    titles->addLayout(titleRow);
    m_pageSubtitle=label("Your next iteration starts with what you learned.","trainingSubtitle");
    m_pageSubtitle->setWordWrap(true); titles->addWidget(m_pageSubtitle); heading->addLayout(titles,1);
    auto *exportRun=new Button("Export run",this); exportRun->setObjectName("trainingExportRun"); exportRun->setMinimumWidth(124); exportRun->setProperty("trainingHeaderAction",true);
    exportRun->setIcon(Theme::coloredIcon("file-up",QColor(Theme::textSecondary()),14)); exportRun->setEnabled(false);
    connect(exportRun,&QPushButton::clicked,this,[this] {
        if (m_displayedJob.isEmpty()) return;
        const auto path=QFileDialog::getSaveFileName(this,"Export training run",QString(),"JSON (*.json)");
        if (path.isEmpty()) return;
        QSaveFile file(path); const auto data=QJsonDocument(m_displayedJob).toJson();
        if (!file.open(QIODevice::WriteOnly) || file.write(data)!=data.size() || !file.commit()) setStatus("Could not export the training run.","error");
        else setStatus("Run measurements and configuration exported.","ok");
    });
    auto *compareAction=new Button("Compare baseline",this); compareAction->setObjectName("trainingCompareAction"); compareAction->setMinimumWidth(125);
    connect(compareAction,&QPushButton::clicked,this,[this] { m_archiveToggle->setChecked(!m_archiveToggle->isChecked()); m_archive->setVisible(true); });
    heading->addWidget(compareAction); compareAction->hide();
    heading->addWidget(exportRun);
    auto *newRun=new Button("+ New run",this); newRun->setObjectName("primaryBtn");
    newRun->setProperty("trainingAction",true); heading->addWidget(newRun); newRun->setMinimumWidth(96); newRun->setProperty("trainingHeaderAction",true);
    connect(newRun,&QPushButton::clicked,this,&TrainingPanel::openRunDialog);
    contentLayout()->addLayout(heading);

    auto *studio=new QWidget(this); studio->setObjectName("trainingStudio");
    m_workspaceGrid=new QGridLayout(studio); m_workspaceGrid->setContentsMargins(0,0,0,0);
    m_workspaceGrid->setHorizontalSpacing(18); m_workspaceGrid->setVerticalSpacing(18);
    contentLayout()->addWidget(studio);
    m_indexCard=new QWidget(this); m_indexCard->setObjectName("trainingExperimentIndex");
    auto *index=new QVBoxLayout(m_indexCard); index->setContentsMargins(0,0,12,0); index->setSpacing(12);
    auto *indexHeading=new QHBoxLayout(); indexHeading->addWidget(label("Experiments","trainingCardHeading"));
    indexHeading->addStretch(); auto *indexNew=new Button("+",this); indexNew->setFlat(true); indexHeading->addWidget(indexNew);
    connect(indexNew,&QPushButton::clicked,this,&TrainingPanel::openRunDialog); index->addLayout(indexHeading);
    index->addWidget(label("LOCAL RUNS","trainingEyebrow"));
    m_runs=new QListWidget(this); m_runs->setObjectName("trainingRuns"); m_runs->setAccessibleName("Training experiment index");
    m_runs->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); m_runs->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel); m_runs->setMinimumHeight(310);
    auto *indexSearch=new QLineEdit(this); indexSearch->setObjectName("trainingIndexSearch");
    indexSearch->setPlaceholderText("Find an experiment…"); indexSearch->setAccessibleName("Find an experiment in Observatory"); indexSearch->setClearButtonEnabled(true);
    index->addWidget(indexSearch);
    m_indexEmpty=label("No experiments yet. Create a new run to begin.","trainingEmptyState"); m_indexEmpty->setWordWrap(true); index->addWidget(m_indexEmpty); m_indexEmpty->hide();
    index->addWidget(m_runs,1); index->addWidget(label("All runs saved on this device","trainingMicro"));
    connect(m_runs,&QListWidget::currentItemChanged,this,[this](QListWidgetItem *item) { if (item) selectRun(item->data(Qt::UserRole).toString()); });
    connect(m_runs,&QListWidget::itemClicked,this,[this](QListWidgetItem *item) { selectRun(item->data(Qt::UserRole).toString()); });
    connect(m_runs,&QListWidget::itemActivated,this,[this](QListWidgetItem *item) { emit runDetailsRequested(item->data(Qt::UserRole).toString()); });

    m_center=new QWidget(this); m_centerLayout=new QVBoxLayout(m_center);
    m_centerLayout->setContentsMargins(0,0,0,0); m_centerLayout->setSpacing(14);
    auto *run=Surface::card(this); run->parentWidget()->setObjectName("trainingSignalBoard");
    run->setContentsMargins(22,22,22,20); run->setSpacing(0); m_centerLayout->addWidget(run->parentWidget());
    auto *session=new QHBoxLayout(); auto *identity=new QVBoxLayout(); identity->setSpacing(5);
    m_runEyebrow=label("READY TO TRAIN","trainingEyebrow"); identity->addWidget(m_runEyebrow);
    m_runName=label("Your next experiment","trainingRunTitle"); m_runName->setWordWrap(true); m_runName->setMinimumWidth(0); m_runName->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred); identity->addWidget(m_runName);
    m_runMeta=label("Choose a model and dataset to begin.","trainingMicro"); m_runMeta->setWordWrap(true); identity->addWidget(m_runMeta);
    session->addLayout(identity,1);
    m_followActive=new Button("View active run",this); m_followActive->setObjectName("trainingFollowActive"); m_followActive->setFlat(true);
    m_followActive->setToolTip("Return to the experiment that is training on this device."); session->addWidget(m_followActive,0,Qt::AlignVCenter); m_followActive->hide();
    connect(m_followActive,&QPushButton::clicked,this,[this] { if (!m_jobId.isEmpty()) { m_selectedRunId=m_jobId; selectRun(m_jobId); onPollStatus(); } });
    auto *statusActions=new QHBoxLayout(); statusActions->setContentsMargins(0,0,0,0); statusActions->setSpacing(6);
    m_badge=label("Ready","trainingBadge"); statusActions->addWidget(m_badge,0,Qt::AlignVCenter); session->addLayout(statusActions);
    run->addLayout(session); run->addSpacing(22);
    auto *stats=new QHBoxLayout(); stats->setSpacing(0);
    auto reading=[this,label,stats](int column,const QString &title,QLabel *&value,const QString &note) {
        auto *metric=new QFrame(this); metric->setObjectName(column==0?"trainingFirstStat":"trainingStat");
        auto *body=new QVBoxLayout(metric); body->setContentsMargins(column==0?0:18,0,12,0); body->setSpacing(6);
        auto *statLabel=label(title,"trainingStatLabel"); body->addWidget(statLabel); if (column==3) m_timeLabel=statLabel; value=label("—",column==0?"trainingStatAccent":"trainingStatValue");
        value->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred); body->addWidget(value);
        auto *detail=label(note,"trainingStatNote"); detail->setWordWrap(true); m_statNotes.append(detail); body->addWidget(detail);
        stats->addWidget(metric,1);
    };
    reading(0,"Training loss",m_lossReading,"Waiting for first sample");
    reading(1,"Validation loss",m_validationReading,"Held-out samples");
    reading(2,"Throughput",m_throughputReading,"steps / second");
    reading(3,"Time remaining",m_remainingReading,"Waiting for timing samples");
    auto *statFrame=new QFrame(this); statFrame->setObjectName("trainingStatsFrame"); auto *statBody=new QVBoxLayout(statFrame);
    statBody->setContentsMargins(0,17,0,8); statBody->addLayout(stats); run->addWidget(statFrame); run->addSpacing(18);
    m_chartTabs=new QWidget(this); m_chartTabs->setObjectName("trainingCharts");
    auto *charts=new QVBoxLayout(m_chartTabs); charts->setContentsMargins(0,0,0,0); charts->setSpacing(0);
    m_metricTabs=new SegmentedTabBar(this); m_metricTabs->setObjectName("trainingMetricTabs"); m_metricTabs->setProperty("underline",true);
    m_metricTabs->setAccessibleName("Training chart metric");
    for (const auto &name : {"Loss","Learning rate","Gradient norm","Throughput"}) m_metricTabs->addTab(name);
    charts->addWidget(m_metricTabs); auto *tools=new QHBoxLayout(); tools->setContentsMargins(0,8,0,3);
    m_legend=label({},"trainingLegend"); m_legend->setTextFormat(Qt::RichText);
    m_legend->setText(QString("<span style='color:%1'>━</span> Training &nbsp; <span style='color:%2'>┄</span> Validation").arg(Theme::accentPrimary(),Theme::statusSuccess())); tools->addWidget(m_legend); tools->addStretch();
    m_smoothing=new ToggleSwitch("Smoothing",this); m_smoothing->setObjectName("trainingSmoothing");
    m_smoothing->setProperty("compact",true); m_smoothing->setChecked(true);
    m_smoothing->setToolTip("Smooth the displayed training curve. Tooltips and exported data keep the original measured values."); tools->addWidget(m_smoothing); charts->addLayout(tools);
    m_chartStack=new QStackedWidget(this); m_chartStack->setObjectName("trainingChartStack"); m_chartStack->setFixedHeight(225);
    m_lossChart=new TrainingChart("loss",this); m_lossChart->setObjectName("trainingLossChart");
    m_lrChart=new TrainingChart("learning_rate",this); m_gradChart=new TrainingChart("grad_norm",this);
    m_throughputChart=new TrainingChart("step_rate",this);
    for (auto *chart : {m_lossChart,m_lrChart,m_gradChart,m_throughputChart}) {
        chart->setSmoothing(true); m_chartStack->addWidget(chart);
    }
    connect(m_metricTabs,&QTabBar::currentChanged,m_chartStack,&QStackedWidget::setCurrentIndex);
    connect(m_metricTabs,&QTabBar::currentChanged,this,[this](int i) { m_legend->setText(i==0?QString("<span style='color:%1'>━</span> Training &nbsp; <span style='color:%2'>┄</span> Validation").arg(Theme::accentPrimary(),Theme::statusSuccess()):QString("<span style='color:%1'>━</span> Measured values").arg(Theme::accentPrimary())); });
    charts->addWidget(m_chartStack); charts->addWidget(label("TRAINING STEPS","trainingAxis")); run->addWidget(m_chartTabs);
    m_rawData=new QPlainTextEdit(this); m_rawData->setObjectName("trainingRawData"); m_rawData->setReadOnly(true);
    m_rawData->setFixedHeight(260); run->addWidget(m_rawData); m_rawData->hide(); run->addSpacing(17);
    auto *progressRow=new QHBoxLayout(); m_stepLabel=label("Step — / —","trainingMono"); m_epochLabel=label("0%","trainingMicro");
    progressRow->addWidget(m_stepLabel); progressRow->addStretch(); progressRow->addWidget(m_epochLabel); run->addLayout(progressRow); run->addSpacing(9);
    m_progress=new ProgressBar(this); m_progress->setRange(0,100); m_progress->setValue(0); m_progress->setFixedHeight(4);
    m_progress->setTextVisible(false); run->addWidget(m_progress); run->addSpacing(16);
    auto *footer=new QFrame(this); footer->setObjectName("trainingMonitorFooter"); auto *footerRow=new QHBoxLayout(footer);
    footerRow->setContentsMargins(0,12,0,0); m_checkpointLabel=label("Training stays on this device","trainingMicro");
    m_checkpointLabel->setWordWrap(true); footerRow->addWidget(m_checkpointLabel,1);
    m_stopButton=new Button("Stop run",this); m_stopButton->setObjectName("trainingStopRun"); m_stopButton->setFlat(true); m_stopButton->setMinimumWidth(74); m_stopButton->setEnabled(false);
    footerRow->addWidget(m_stopButton); run->addWidget(footer);
    m_progressLabel=label("Ready to train","metricValue"); m_progressLabel->hide();
    m_metrics=label("Waiting for a training job.","cardMeta"); m_metrics->hide();
    m_activity=new StudioCanvas(this); m_activity->setObjectName("trainingActivity"); m_activity->setProperty("brandMark",true);
    m_activity->setProperty("activityIndicator",true); m_activity->setMotionEnabled(false); m_activity->hide();
    m_runHint=label({},"cardMeta"); m_runHint->hide();

    m_observatorySignals=new QWidget(this); m_observatorySignals->setObjectName("trainingObservatorySignals");
    auto *smallSignals=new QHBoxLayout(m_observatorySignals); smallSignals->setContentsMargins(0,0,0,0); smallSignals->setSpacing(14);
    auto smallChart=[this,label,smallSignals](const QString &title,const QString &metric,QLabel *&value,const QString &hint) {
        auto *card=Surface::card(this); card->parentWidget()->setObjectName("trainingMiniCard"); card->setContentsMargins(15,15,15,10);
        auto *heading=new QHBoxLayout(); heading->addWidget(label(title,"trainingCardHeading")); heading->addStretch();
        value=label("—","trainingAccentMicro"); heading->addWidget(value); card->addLayout(heading);
        card->addWidget(label(hint,"trainingMicro")); auto *chart=new TrainingChart(metric,this); chart->setFixedHeight(120);
        chart->setSmoothing(true); card->addWidget(chart); smallSignals->addWidget(card->parentWidget(),1); return chart;
    };
    m_observatoryRate=smallChart("Learning rate","learning_rate",m_rateReading,"Recorded optimizer schedule");
    m_observatoryGradient=smallChart("Gradient norm","grad_norm",m_gradientReading,"Measured update magnitude");
    m_centerLayout->addWidget(m_observatorySignals); m_observatorySignals->hide();
    connect(m_smoothing,&QCheckBox::toggled,this,[this](bool enabled) {
        for (auto *chart : {m_lossChart,m_lrChart,m_gradChart,m_throughputChart,m_observatoryRate,m_observatoryGradient}) chart->setSmoothing(enabled);
    });
    auto *events=Surface::card(this); events->parentWidget()->setObjectName("trainingLogCard"); events->setContentsMargins(12,5,12,10); events->setSpacing(8);
    auto *logHeader=new QHBoxLayout(); logHeader->setSpacing(8);
    m_logToggle=new Button("▸ Event log",this); m_logToggle->setProperty("leftAligned",true);
    m_logToggle->setObjectName("trainingLogToggle"); m_logToggle->setCheckable(true); m_logToggle->setFlat(true); m_logToggle->setMinimumWidth(104);
    m_logToggle->setAccessibleName("Expand event log"); logHeader->addWidget(m_logToggle);
    m_logCount=label("0 events","trainingMicro"); logHeader->addWidget(m_logCount); logHeader->addStretch();
    m_logFollow=new Button("Follow latest",this); m_logFollow->setObjectName("trainingLogFollow"); m_logFollow->setCheckable(true); m_logFollow->setChecked(true);
    m_logFollow->setToolTip("Keep new events in view. Scrolling up pauses following."); logHeader->addWidget(m_logFollow);
    m_logCopy=new Button("Copy",this); m_logCopy->setObjectName("trainingLogCopy"); m_logCopy->setAccessibleName("Copy visible event log");
    logHeader->addWidget(m_logCopy); events->addLayout(logHeader);
    m_logBody=new QWidget(this); auto *logBodyLayout=new QVBoxLayout(m_logBody); logBodyLayout->setContentsMargins(0,0,0,0); logBodyLayout->setSpacing(8);
    m_logSearch=new QLineEdit(this); m_logSearch->setObjectName("trainingLogSearch"); m_logSearch->setPlaceholderText("Filter events by message or level…");
    m_logSearch->setAccessibleName("Filter event log"); m_logSearch->setClearButtonEnabled(true); logBodyLayout->addWidget(m_logSearch);
    m_log=new QPlainTextEdit(this); m_log->setObjectName("trainingEventLog"); m_log->setAccessibleName("Events for the selected experiment");
    m_log->setReadOnly(true); m_log->setMaximumBlockCount(2000); m_log->setLineWrapMode(QPlainTextEdit::NoWrap);
    m_log->setPlaceholderText("Events will appear when this experiment starts."); m_log->setFixedHeight(190); logBodyLayout->addWidget(m_log);
    new TrainingLogHighlighter(m_log->document()); events->addWidget(m_logBody); m_logBody->hide(); m_logFollow->hide(); m_logCopy->hide();
    connect(m_logToggle,&QPushButton::toggled,this,[this](bool open) {
        m_logBody->setVisible(open); m_logFollow->setVisible(open); m_logCopy->setVisible(open);
        m_logToggle->setText(open?"▾ Event log":"▸ Event log"); m_logToggle->setAccessibleName(open?"Collapse event log":"Expand event log");
        if (open) renderEventLog();
    });
    connect(m_logSearch,&QLineEdit::textChanged,this,[this] { renderEventLog(); });
    connect(m_logCopy,&QPushButton::clicked,this,[this] { QApplication::clipboard()->setText(m_log->toPlainText()); });
    connect(m_logFollow,&QPushButton::toggled,this,[this](bool follow) { if (follow) m_log->verticalScrollBar()->setValue(m_log->verticalScrollBar()->maximum()); });
    connect(m_log->verticalScrollBar(),&QScrollBar::valueChanged,this,[this](int value) {
        if (!m_renderingLog && value<m_previousLogScroll && value<m_log->verticalScrollBar()->maximum()) m_logFollow->setChecked(false);
        m_previousLogScroll=value;
    });
    m_centerLayout->addWidget(events->parentWidget());
    m_centerLayout->addStretch();

    m_rail=new QWidget(this); m_railLayout=new QVBoxLayout(m_rail); m_railLayout->setContentsMargins(0,0,0,0); m_railLayout->setSpacing(16);
    auto *compute=Surface::card(this); m_computeCard=compute->parentWidget(); m_computeCard->setObjectName("trainingComputeCard");
    compute->setContentsMargins(18,18,18,18); compute->setSpacing(14);
    auto *computeHead=new QHBoxLayout(); computeHead->addWidget(label("Local compute","trainingCardHeading")); computeHead->addStretch();
    m_computeValues["accelerator"]=label("LOCAL","trainingBadge"); computeHead->addWidget(m_computeValues["accelerator"]); compute->addLayout(computeHead);
    auto *deviceRow=new QHBoxLayout(); auto *chip=label({},"trainingChip"); chip->setPixmap(Theme::iconPixmap("cpu",QColor(Theme::accentPrimary()),26,devicePixelRatioF())); chip->setFixedSize(28,32); deviceRow->addWidget(chip);
    auto *deviceTitle=new QVBoxLayout(); deviceTitle->setSpacing(3); m_computeValues["device"]=label("Local device","trainingDeviceName");
    m_computeValues["device"]->setWordWrap(true); deviceTitle->addWidget(m_computeValues["device"]);
    m_computeValues["capacity"]=label("Reading device telemetry…","trainingMicro"); deviceTitle->addWidget(m_computeValues["capacity"]);
    deviceRow->addLayout(deviceTitle,1);
    auto *deviceFrame=new QFrame(this); deviceFrame->setObjectName("trainingDeviceFrame");
    auto *deviceBody=new QVBoxLayout(deviceFrame); deviceBody->setContentsMargins(0,0,0,17); deviceBody->addLayout(deviceRow); compute->addWidget(deviceFrame);
    auto metricRow=[this,label,compute](const QString &name,const QString &key) {
        auto *row=new QHBoxLayout(); row->addWidget(label(name,"trainingMicro")); row->addStretch();
        auto *value=label("—","trainingMono"); m_computeValues[key]=value; row->addWidget(value); compute->addLayout(row);
    };
    metricRow("GPU utilization","utilization");
    auto *util=new QHBoxLayout(); util->setSpacing(3); util->setContentsMargins(0,0,0,3);
    for (int i=0;i<32;++i) { auto *bar=new QFrame(this); bar->setObjectName("trainingUtilBar"); bar->setFixedHeight(22); util->addWidget(bar,1); m_utilSegments.append(bar); }
    compute->addLayout(util); metricRow("Memory allocation","memory");
    auto *deviceMemory=new ProgressBar(this); deviceMemory->setObjectName("trainingDeviceMemory"); deviceMemory->setRange(0,100);
    deviceMemory->setTextVisible(false); deviceMemory->setFixedHeight(6); compute->addWidget(deviceMemory);
    m_computeValues["memoryDetail"]=label("Device-wide usage                     Free —","trainingTiny"); compute->addWidget(m_computeValues["memoryDetail"]);
    auto *thermal=new QFrame(this); thermal->setObjectName("trainingThermal"); auto *thermalRow=new QHBoxLayout(thermal);
    thermalRow->setContentsMargins(0,12,0,0); m_computeValues["temperature"]=label("Temperature   —","trainingMicro");
    m_computeValues["power"]=label("Power   —","trainingMicro"); thermalRow->addWidget(m_computeValues["temperature"]); thermalRow->addStretch(); thermalRow->addWidget(m_computeValues["power"]);
    compute->addWidget(thermal);
    auto *configuration=Surface::card(this); m_configCard=configuration->parentWidget(); m_configCard->setObjectName("trainingRunContext");
    configuration->setContentsMargins(18,18,18,18); configuration->setSpacing(15);
    auto *configHead=new QHBoxLayout(); configHead->addWidget(label("Run configuration","trainingCardHeading")); configHead->addStretch();
    configHead->addWidget(label("READ ONLY","trainingTiny")); configuration->addLayout(configHead);
    m_configValues["model"]=label("No experiment selected","trainingDeviceName"); m_configValues["model"]->setWordWrap(true);
    auto *modelIdentity=new QHBoxLayout(); auto *modelIcon=new QLabel(this);
    modelIcon->setProperty("trainingIcon","box"); modelIcon->setPixmap(Theme::iconPixmap("box",QColor(Theme::textSecondary()),16,devicePixelRatioF())); modelIdentity->addWidget(modelIcon);
    modelIdentity->addWidget(m_configValues["model"],1); configuration->addLayout(modelIdentity);
    m_configValues["dataset"]=label("—","trainingMicro"); m_configValues["dataset"]->setWordWrap(true);
    auto *datasetIdentity=new QHBoxLayout(); auto *datasetIcon=new QLabel(this);
    datasetIcon->setProperty("trainingIcon","database"); datasetIcon->setPixmap(Theme::iconPixmap("database",QColor(Theme::textMuted()),14,devicePixelRatioF())); datasetIdentity->addWidget(datasetIcon);
    datasetIdentity->addWidget(m_configValues["dataset"],1); configuration->addLayout(datasetIdentity);
    auto *configGrid=new QGridLayout(); configGrid->setHorizontalSpacing(12); configGrid->setVerticalSpacing(5);
    const QList<QPair<QString,QString>> configFields{{"method","Method"},{"rank","Rank / alpha"},{"lr","Learning rate"},{"batch","Batch size"},{"sequence","Sequence"},{"validation","Validation"}};
    for (int i=0;i<configFields.size();++i) {
        const int row=(i/2)*3,col=i%2; configGrid->addWidget(label(configFields[i].second,"trainingMicro"),row,col);
        m_configValues[configFields[i].first]=label("—","trainingMono"); configGrid->addWidget(m_configValues[configFields[i].first],row+1,col); configGrid->setRowMinimumHeight(row+2,9);
        configGrid->setColumnStretch(col,1);
    }
    configuration->addLayout(configGrid);
    auto *reuse=new Button("Use as next run     →",this); reuse->setObjectName("trainingReuseRun"); reuse->setEnabled(false);
    configuration->addWidget(reuse); connect(reuse,&QPushButton::clicked,this,&TrainingPanel::reuseRun);
    auto *review=new Button("Review run",this); review->setObjectName("trainingReviewRun"); review->setFlat(true); review->setEnabled(false);
    connect(review,&QPushButton::clicked,this,[this,review] { emit runDetailsRequested(review->property("jobId").toString()); }); review->hide();
    m_visualise=new ToggleSwitch("Visualisation",this); m_visualise->setProperty("compact",true); m_visualise->setChecked(true); m_visualise->hide();
    auto *actionsButton=new Button("⋯",this); actionsButton->setObjectName("trainingRunActions"); actionsButton->setFlat(true); actionsButton->setFixedSize(32,28);
    actionsButton->setAccessibleName("Run actions"); actionsButton->setToolTip("Run actions · raw data and full review"); statusActions->addWidget(actionsButton,0,Qt::AlignVCenter);
    connect(actionsButton,&QPushButton::clicked,this,[this,actionsButton,run] {
        emit run->parentWidget()->customContextMenuRequested(run->parentWidget()->mapFromGlobal(actionsButton->mapToGlobal(QPoint(0,actionsButton->height()))));
    });
    run->parentWidget()->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(run->parentWidget(),&QWidget::customContextMenuRequested,this,[this,run,review](const QPoint &point) {
        QMenu menu(this); auto *raw=menu.addAction("Raw job data"); raw->setCheckable(true); raw->setChecked(!m_visualise->isChecked());
        auto *detail=menu.addAction("Review full run"); detail->setEnabled(review->isEnabled());
        const auto chosen=menu.exec(run->parentWidget()->mapToGlobal(point));
        if (chosen==raw) m_visualise->setChecked(!m_visualise->isChecked());
        if (chosen==detail) review->click();
    });
    m_runConfiguration=label({},"trainingConfiguration"); m_runConfiguration->hide();
    auto *activity=Surface::card(this,"Run activity"); m_activityCard=activity->parentWidget(); m_activityCard->setObjectName("trainingActivityCard");
    activity->setContentsMargins(18,18,18,18); m_activityText=label("Waiting for a training experiment.","trainingTimeline");
    m_activityText->setWordWrap(true); m_activityText->setAlignment(Qt::AlignTop); m_activityText->setMinimumHeight(180); activity->addWidget(m_activityText);
    m_railLayout->addWidget(m_computeCard); m_railLayout->addWidget(m_configCard); m_railLayout->addWidget(m_activityCard); m_railLayout->addStretch(); m_activityCard->hide();

    // Build the existing native editors once; the new-run dialog reuses them.
    m_startButton=new Button("Start Training",this); m_startButton->setObjectName("primaryBtn");
    m_startButton->setIcon(Theme::coloredIcon("play",QColor(Theme::accentText()),14));
    auto *recipe=Surface::card(this); recipe->setContentsMargins(20,17,20,15);
    m_setup=recipe->parentWidget(); m_setup->setObjectName("trainingRecipe");
    contentLayout()->addWidget(m_setup);
    auto *recipeHeading=new QHBoxLayout();
    auto *recipeTitle=new QLabel("Next run recipe",this); recipeTitle->setObjectName("headingLabel");
    recipeTitle->setObjectName("trainingRecipeTitle"); recipeHeading->addWidget(recipeTitle); recipeHeading->addStretch();
    m_settingsToggle=new Button("Edit settings",this); m_settingsToggle->setCheckable(true);
    m_settingsToggle->setObjectName("trainingSettingsToggle"); m_settingsToggle->setFlat(true); m_settingsToggle->setMinimumWidth(108);
    recipeHeading->addWidget(m_settingsToggle); recipe->addLayout(recipeHeading);
    auto *recipeHint=new QLabel("Prepare your next iteration from a saved local recipe.",this);
    recipeHint->setObjectName("trainingMicro"); recipeHint->setWordWrap(true);
    recipeHeading->removeWidget(recipeTitle); auto *recipeIdentity=new QVBoxLayout(); recipeIdentity->setSpacing(4);
    recipeIdentity->addWidget(recipeTitle); recipeIdentity->addWidget(recipeHint); recipeHeading->insertLayout(0,recipeIdentity);
    auto *recipeMark=new QLabel(this); recipeMark->setObjectName("trainingRecipeMark"); recipeMark->setFixedSize(34,34);
    recipeMark->setProperty("trainingIcon","layers"); recipeMark->setAlignment(Qt::AlignCenter); recipeMark->setPixmap(Theme::iconPixmap("layers",QColor(Theme::accentLabel()),18,devicePixelRatioF()));
    recipeHeading->insertWidget(0,recipeMark); recipeHeading->setStretch(1,1); recipeHeading->setStretch(2,0);
    auto *setup=m_setup;
    m_recipeInputs=new QWidget(this); m_recipeInputs->setObjectName("trainingRecipeEditors");
    auto *editorLayout=new QVBoxLayout(m_recipeInputs); editorLayout->setContentsMargins(0,0,0,0);
    auto *inputs=new QHBoxLayout(); inputs->setSpacing(12); editorLayout->addLayout(inputs);
    recipe->addWidget(m_recipeInputs); m_recipeInputs->hide();
    auto *model = Surface::card(setup,"01 / BASE MODEL");
    model->parentWidget()->setObjectName("recipeNode");
    inputs->addWidget(model->parentWidget(),2);
    m_modelCombo = new ComboBox(this);
    m_modelCombo->setEditable(true);
    m_modelCombo->setMinimumContentsLength(10);
    m_modelCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_modelCombo->setInsertPolicy(QComboBox::NoInsert);
    m_modelCombo->lineEdit()->setPlaceholderText("Choose a model or enter its ID");
    m_modelCombo->setAccessibleName("Base model");
    model->addWidget(m_modelCombo);
    auto *modelRow = new QHBoxLayout();
    auto *refreshBtn = new Button("Refresh", this);
    refreshBtn->setObjectName("secondaryBtn");
    auto *folderBtn = new Button("Folder...", this);
    folderBtn->setObjectName("secondaryBtn");
    modelRow->addWidget(refreshBtn);
    modelRow->addWidget(folderBtn);
    modelRow->addStretch();
    model->addLayout(modelRow);
    auto *modelHint = new QLabel("Training needs original Transformers weights; GGUF is for inference.",this);
    modelHint->setObjectName("cardMeta"); modelHint->setWordWrap(true);
    m_modelCombo->setToolTip(modelHint->text());
    modelHint->hide();
    model->addStretch();
    connect(refreshBtn, &QPushButton::clicked, this, &TrainingPanel::refreshOptions);
    connect(folderBtn, &QPushButton::clicked, this, &TrainingPanel::onBrowseBaseModel);

    auto *dataset = Surface::card(setup,"02 / DATASET");
    dataset->parentWidget()->setObjectName("recipeNode");
    inputs->addWidget(dataset->parentWidget(),2);
    m_datasetCombo = new ComboBox(this);
    m_datasetCombo->setEditable(true);
    m_datasetCombo->setMinimumContentsLength(10);
    m_datasetCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_datasetCombo->setInsertPolicy(QComboBox::NoInsert);
    m_datasetCombo->lineEdit()->setPlaceholderText("Choose an imported dataset");
    m_datasetCombo->setAccessibleName("Dataset");
    auto *dataRow=new QHBoxLayout(); dataRow->addWidget(m_datasetCombo,1);
    dataset->addLayout(dataRow);
    auto *ingestBtn = new Button("Import file...", this);
    ingestBtn->setObjectName("secondaryBtn");

    dataRow->addWidget(ingestBtn);
    connect(ingestBtn, &QPushButton::clicked, this, &TrainingPanel::onBrowseDataset);
    m_datasetSummary = new QLabel("No dataset selected.",this);
    m_datasetSummary->setObjectName("cardMeta"); m_datasetSummary->setWordWrap(true);
    dataset->addWidget(m_datasetSummary);
    dataset->addStretch();
    m_validationSpin = new DoubleSpinBox(this);
    m_validationSpin->setRange(0,50); m_validationSpin->setDecimals(0);
    m_validationSpin->setSingleStep(5); m_validationSpin->setSuffix("%");
    m_validationSpin->setToolTip("Hold out this fraction of rows and measure validation loss after each epoch. 0% disables validation.");


    setupHyperparameters(setup);
    auto *methodNode=Surface::card(setup,"03 / METHOD");
    methodNode->parentWidget()->setObjectName("recipeNode");
    auto *method=findChild<QComboBox *>("trainingMethod");
    method->setMinimumContentsLength(8);
    method->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    auto *hyperGrid=m_hyperBox->findChild<QGridLayout *>();
    hyperGrid->removeWidget(method); methodNode->addWidget(method);
    auto *oldMethodLabel=hyperGrid->itemAtPosition(6,0)->widget();
    hyperGrid->removeWidget(oldMethodLabel); delete oldMethodLabel;
    auto *precisionLabel=hyperGrid->itemAtPosition(6,1)->widget();
    hyperGrid->removeWidget(precisionLabel); hyperGrid->removeWidget(m_quantCombo);
    hyperGrid->addWidget(precisionLabel,6,0); hyperGrid->addWidget(m_quantCombo,7,0);
    auto *validationLabel=new QLabel("Held-out validation",m_hyperBox);
    validationLabel->setObjectName("cardMeta"); validationLabel->setBuddy(m_validationSpin);
    m_validationSpin->setAccessibleName("Held-out validation");
    hyperGrid->addWidget(validationLabel,6,1); hyperGrid->addWidget(m_validationSpin,7,1);
    auto *methodHint=new QLabel("Local adapter training",this); methodHint->setObjectName("cardMeta");
    methodNode->addWidget(methodHint); methodNode->addStretch();
    inputs->addWidget(methodNode->parentWidget(),1);
    editorLayout->addWidget(m_hyperBox); m_hyperBox->hide();
    connect(m_settingsToggle,&QPushButton::toggled,this,[this](bool open) {
        m_recipeInputs->setVisible(open); m_hyperBox->setVisible(open);
        m_startButton->setVisible(open);
        m_settingsToggle->setText(open ? "Close settings" : "Edit settings");
    });
    auto *outputOptions = new Button("Output options ▾",this);
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
    auto *resources=Surface::card(this,"RESOURCE ESTIMATE");
    resources->parentWidget()->setObjectName("trainingBudget");
    editorLayout->addWidget(resources->parentWidget());
    m_inputSummary = new QLabel(this);
    m_inputSummary->setObjectName("cardMeta"); m_inputSummary->setWordWrap(true);
    m_inputSummary->setTextFormat(Qt::PlainText);
    m_inputSummary->hide();
    m_estimateLabel=new QLabel("Select a model and dataset to estimate resources.",this);
    m_estimateLabel->setObjectName("cardMeta"); m_estimateLabel->setWordWrap(true);
    m_estimateLabel->setTextFormat(Qt::PlainText); resources->addWidget(m_estimateLabel);
    m_memoryBar=new ProgressBar(this); m_memoryBar->setRange(0,100);
    m_memoryBar->setFixedHeight(6); m_memoryBar->setTextVisible(false);
    resources->addWidget(m_memoryBar); m_memoryBar->hide();
    m_estimateTimer=new QTimer(this); m_estimateTimer->setSingleShot(true); m_estimateTimer->setInterval(300);
    connect(m_estimateTimer,&QTimer::timeout,this,&TrainingPanel::requestEstimate);
    auto *presets = new QHBoxLayout();
    auto *savePreset = new Button("Save preset…",this);
    auto *loadPreset = m_loadPreset = new Button("Load preset…",this);
    presets->addWidget(savePreset); presets->addWidget(loadPreset); presets->addStretch();
    outputLayout->addLayout(presets);

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



    m_recipeSummary=label("Choose a base model  →  Select your dataset  →  LoRA", "trainingRecipeSummary");
    m_recipeSummary->setWordWrap(true);
    auto *recipeFooter=new QHBoxLayout(); m_recipeFacts=label({},"trainingMicro"); m_recipeFacts->setWordWrap(true);
    recipeFooter->addWidget(m_recipeFacts,1); m_budgetSummary=label("Resource estimate pending","trainingMicro");
    m_budgetSummary->setWordWrap(true); recipeFooter->addWidget(m_budgetSummary); recipe->addLayout(recipeFooter);
    auto *recipeActions=new QHBoxLayout(); auto *configure=new Button("Configure && start ↗",this); configure->setFlat(true);
    configure->setObjectName("trainingConfigureRun"); connect(configure,&QPushButton::clicked,this,&TrainingPanel::openRunDialog);
    configure->setMinimumWidth(146);
    auto *summaryRow=new QHBoxLayout(); summaryRow->addWidget(m_recipeSummary,1); summaryRow->addWidget(configure); recipe->insertLayout(2,summaryRow);
    recipeActions->addStretch(); recipeActions->addWidget(m_startButton); recipe->addLayout(recipeActions); m_startButton->hide();
    m_runDialog=new QDialog(this); m_runDialog->setObjectName("trainingRunDialog"); m_runDialog->setWindowTitle("Configure your next run");
    m_runDialog->setModal(true); m_runDialog->resize(700,700); m_dialogLayout=new QVBoxLayout(m_runDialog); m_dialogLayout->setContentsMargins(12,12,12,12);
    connect(m_runDialog,&QDialog::finished,this,&TrainingPanel::closeRunDialog);
    m_archive=new QWidget(this); m_archive->setObjectName("trainingArchive"); auto *archive=new QVBoxLayout(m_archive);
    archive->setContentsMargins(0,0,0,0); archive->setSpacing(14); contentLayout()->addWidget(m_archive);
    auto *archiveHead=new QHBoxLayout(); auto *archiveTitles=new QVBoxLayout(); archiveTitles->setSpacing(4);
    archiveTitles->addWidget(label("Experiments","trainingSectionTitle")); archiveTitles->addWidget(label("A local record of every iteration.","trainingMicro"));
    archiveHead->addLayout(archiveTitles); archiveHead->addStretch();
    auto *search=new QLineEdit(this); search->setObjectName("trainingRunSearch"); search->setPlaceholderText("Find an experiment…");
    search->setAccessibleName("Find an experiment"); search->setClearButtonEnabled(true); search->setMaximumWidth(190); archiveHead->addWidget(search);
    connect(search,&QLineEdit::textChanged,this,[this,indexSearch](const QString &text) {
        const QSignalBlocker blocker(indexSearch); indexSearch->setText(text); filterRuns();
    });
    connect(indexSearch,&QLineEdit::textChanged,this,[this,search](const QString &text) {
        const QSignalBlocker blocker(search); search->setText(text); filterRuns();
    });
    m_archiveToggle=new Button("Compare baseline",this); m_archiveToggle->setObjectName("trainingArchiveToggle");
    m_archiveToggle->setCheckable(true); archiveHead->addWidget(m_archiveToggle); archive->addLayout(archiveHead);
    m_experiments=new QTableWidget(this); m_experiments->setObjectName("trainingExperimentTable");
    QFont tableFont=m_experiments->font(); tableFont.setPixelSize(10); m_experiments->setFont(tableFont);
    m_experiments->setColumnCount(6); m_experiments->setHorizontalHeaderLabels({"EXPERIMENT","BASE MODEL","TRAIN LOSS","VAL LOSS","STATUS",""});
    m_experiments->verticalHeader()->hide(); m_experiments->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft|Qt::AlignVCenter); m_experiments->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    m_experiments->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch);
    m_experiments->horizontalHeader()->setSectionResizeMode(1,QHeaderView::Stretch);
    m_experiments->horizontalHeader()->setSectionResizeMode(5,QHeaderView::Fixed); m_experiments->setColumnWidth(5,38);
    m_experiments->setEditTriggers(QAbstractItemView::NoEditTriggers); m_experiments->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_experiments->setSelectionMode(QAbstractItemView::SingleSelection); m_experiments->setShowGrid(false);
    m_experiments->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); m_experiments->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel); m_experiments->setMinimumHeight(100);
    archive->addWidget(m_experiments);
    m_historyEmpty=label("No experiments yet. Create a new run to begin.","trainingEmptyState");
    m_historyEmpty->setWordWrap(true); archive->addWidget(m_historyEmpty); m_historyEmpty->hide();
    connect(m_experiments,&QTableWidget::cellClicked,this,[this](int row,int col) {
        const auto id=m_experiments->item(row,0)->data(Qt::UserRole).toString();
        selectRun(id); if (col==5) reuseRun();
    });
    connect(m_experiments,&QTableWidget::currentCellChanged,this,[this](int row,int,int,int) {
        if (row>=0 && m_experiments->item(row,0)) selectRun(m_experiments->item(row,0)->data(Qt::UserRole).toString());
    });
    connect(m_experiments,&QTableWidget::cellDoubleClicked,this,[this](int row,int) { emit runDetailsRequested(m_experiments->item(row,0)->data(Qt::UserRole).toString()); });
    m_comparisonPanel=new QWidget(this); auto *comparisonLayout=new QVBoxLayout(m_comparisonPanel); comparisonLayout->setContentsMargins(0,0,0,0);
    auto *runsPage=m_comparisonPanel;
    auto *compare = Surface::card(runsPage,"Compare runs");
    comparisonLayout->addWidget(compare->parentWidget());
    auto *selectors = new QHBoxLayout();
    m_compareA=new ComboBox(this); m_compareA->setObjectName("compareRunA");
    m_compareB=new ComboBox(this); m_compareB->setObjectName("compareRunB");
    for (auto *combo : {m_compareA,m_compareB}) {
        combo->setMinimumContentsLength(8);
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        selectors->addWidget(combo,1);
        connect(combo,&QComboBox::currentIndexChanged,this,&TrainingPanel::updateComparison);
    }
    m_compareA->setAccessibleName("Run A, orange"); m_compareB->setAccessibleName("Run B, blue");
    compare->addLayout(selectors);
    m_comparisonHint=new QLabel("Choose two runs to compare measured results and settings.",this);
    m_comparisonHint->setWordWrap(true); m_comparisonHint->setObjectName("cardMeta");
    compare->addWidget(m_comparisonHint);
    auto *comparisonCharts=new QHBoxLayout();
    auto comparisonChart=[this,comparisonCharts](const QString &title,const QString &metric) {
        auto *card=Surface::card(this,title);
        auto *chart=new TrainingChart(metric,this); chart->setMinimumHeight(180);
        card->addWidget(chart); comparisonCharts->addWidget(card->parentWidget(),1); return chart;
    };
    m_compareLoss=comparisonChart("Loss","loss"); m_compareLoss->setObjectName("comparisonLossChart");
    m_compareRate=comparisonChart("Learning rate","learning_rate");
    m_compareGradient=comparisonChart("Gradient norm","grad_norm");
    compare->addLayout(comparisonCharts);
    m_comparisonTable=new QTableWidget(this); m_comparisonTable->setObjectName("trainingComparison");
    m_comparisonTable->setColumnCount(3);
    m_comparisonTable->setHorizontalHeaderLabels({"Measurement / setting","Run A","Run B"});
    m_comparisonTable->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    m_comparisonTable->verticalHeader()->hide();
    m_comparisonTable->setEditTriggers(QAbstractItemView::NoEditTriggers); m_comparisonTable->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_comparisonTable->setWordWrap(true); m_comparisonTable->setMinimumHeight(260);
    m_comparisonTable->setAccessibleName("Run measurements and configuration comparison");
    compare->addWidget(m_comparisonTable);


    archive->addWidget(m_comparisonPanel); m_comparisonPanel->hide();
    m_compareSummary=label({},"trainingComparisonSummary"); run->insertWidget(3,m_compareSummary); m_compareSummary->hide();
    connect(m_archiveToggle,&QPushButton::toggled,this,[this](bool open) {
        m_archive->setVisible(m_workspaceTabs->currentIndex()==0 || open);
        m_comparisonPanel->setVisible(open && m_workspaceTabs->currentIndex()==1); m_compareSummary->setVisible(open && m_workspaceTabs->currentIndex()==1);
        if (open && m_compareA->count()>2 && m_compareA->currentIndex()==0) { m_compareA->setCurrentIndex(1); m_compareB->setCurrentIndex(2); }
        if (!open && !m_displayedJob.isEmpty()) renderJob(m_displayedJob);
        else updateComparison();
    });
    m_statusLabel=label("Ready. Select a base model and an imported dataset.","statusLabel"); m_statusLabel->setWordWrap(true);
    m_feedbackFrame=new QFrame(this); m_feedbackFrame->setObjectName("trainingFeedback");
    auto *feedbackRow=new QHBoxLayout(m_feedbackFrame); feedbackRow->setContentsMargins(12,6,6,6); feedbackRow->setSpacing(8);
    feedbackRow->addWidget(m_statusLabel,1); auto *dismiss=new Button("×",this); dismiss->setObjectName("trainingDismissStatus");
    dismiss->setAccessibleName("Dismiss notification"); dismiss->setToolTip("Dismiss notification"); dismiss->setFlat(true); dismiss->setFixedWidth(28);
    feedbackRow->addWidget(dismiss); connect(dismiss,&QPushButton::clicked,this,[this] { setStatus({}, {},false); });
    contentLayout()->insertWidget(2,m_feedbackFrame); m_feedbackFrame->hide();
    auto *footnote=label("MAGNITE / YOUR MODELS. YOUR MACHINE.                                               LOCAL TRAINING", "trainingTiny");
    contentLayout()->addWidget(footnote); contentLayout()->addStretch(1);
    m_pollTimer=new QTimer(this); m_pollTimer->setInterval(kPollMs); connect(m_pollTimer,&QTimer::timeout,this,&TrainingPanel::onPollStatus);
    connect(m_startButton,&QPushButton::clicked,this,&TrainingPanel::onStart); connect(m_stopButton,&QPushButton::clicked,this,&TrainingPanel::onStop);
    connect(m_workspaceTabs,&QTabBar::currentChanged,this,&TrainingPanel::setWorkspace);
    connect(m_visualise,&QCheckBox::toggled,this,[this](bool enabled) {
        m_chartTabs->setVisible(enabled); m_rawData->setVisible(!enabled);
        m_observatorySignals->setVisible(enabled && m_workspaceTabs->currentIndex()==1);
    });
    connect(m_modelCombo,&QComboBox::currentTextChanged,this,&TrainingPanel::updateInputSummary);
    connect(m_datasetCombo,&QComboBox::currentTextChanged,this,&TrainingPanel::updateInputSummary);
    for (auto *spin : {m_rankSpin,m_alphaSpin,m_maxLengthSpin,m_batchSpin,m_epochsSpin}) connect(spin,&QSpinBox::valueChanged,this,&TrainingPanel::updateInputSummary);
    connect(m_validationSpin,&QDoubleSpinBox::valueChanged,this,&TrainingPanel::updateInputSummary);
    connect(m_quantCombo,&QComboBox::currentIndexChanged,this,&TrainingPanel::updateInputSummary);
    connect(m_targetModules,&QComboBox::currentIndexChanged,this,&TrainingPanel::updateInputSummary);
    connect(m_lrSpin,&QDoubleSpinBox::valueChanged,this,&TrainingPanel::updateRecipeSummary);
    m_dialogEstimateTimer=new QTimer(this); m_dialogEstimateTimer->setSingleShot(true); m_dialogEstimateTimer->setInterval(300);
    connect(m_dialogEstimateTimer,&QTimer::timeout,this,&TrainingPanel::requestDialogEstimate);
    m_telemetryTimer=new QTimer(this); m_telemetryTimer->setInterval(3000);
    connect(m_telemetryTimer,&QTimer::timeout,this,[this] { if (isVisible()) refreshTelemetry(); }); m_telemetryTimer->start();
    updateInputSummary(); setWorkspace(0); wireScrolling(this);
}

void TrainingPanel::setupHyperparameters(QWidget *parent) {
    auto *card=Surface::card(parent,"Training settings");
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
    m_rankSpin=new SpinBox(m_hyperBox); m_rankSpin->setRange(1,512); m_rankSpin->setValue(8);
    m_alphaSpin=new SpinBox(m_hyperBox); m_alphaSpin->setRange(1,1024); m_alphaSpin->setValue(16);
    m_dropoutSpin=new DoubleSpinBox(m_hyperBox); m_dropoutSpin->setRange(0,0.99);
    m_dropoutSpin->setDecimals(2); m_dropoutSpin->setSingleStep(0.01); m_dropoutSpin->setValue(0.05);
    field(0,0,"Rank",m_rankSpin); field(0,1,"Alpha",m_alphaSpin); field(0,2,"Dropout",m_dropoutSpin);
    m_epochsSpin=new SpinBox(m_hyperBox); m_epochsSpin->setRange(1,100); m_epochsSpin->setValue(3);
    m_lrSpin=new DoubleSpinBox(m_hyperBox); m_lrSpin->setRange(0.000001,1);
    m_lrSpin->setDecimals(6); m_lrSpin->setSingleStep(0.00001); m_lrSpin->setValue(0.0002);
    m_batchSpin=new SpinBox(m_hyperBox); m_batchSpin->setRange(1,128); m_batchSpin->setValue(1);
    field(1,0,"Epochs",m_epochsSpin); field(1,1,"Learning rate",m_lrSpin); field(1,2,"Batch size",m_batchSpin);
    m_targetModules=new ComboBox(m_hyperBox);
    m_targetModules->setMinimumContentsLength(5);
    m_targetModules->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_targetModules->addItems({"q_proj,v_proj","q_proj,k_proj,v_proj,o_proj","all-linear"});
    m_maxLengthSpin=new SpinBox(m_hyperBox); m_maxLengthSpin->setRange(8,8192); m_maxLengthSpin->setValue(512);
    m_warmupSpin=new DoubleSpinBox(m_hyperBox); m_warmupSpin->setRange(0,1);
    m_warmupSpin->setDecimals(2); m_warmupSpin->setSingleStep(0.01); m_warmupSpin->setValue(0.03);
    field(2,0,"Target modules",m_targetModules); field(2,1,"Max length",m_maxLengthSpin); field(2,2,"Warmup ratio",m_warmupSpin);
    m_quantCombo=new ComboBox(m_hyperBox);
    m_quantCombo->addItems({"None","4-bit (NF4)","8-bit (bitsandbytes)"});
    m_quantCombo->setAccessibleName("Quantization");
    auto *method=new ComboBox(m_hyperBox); method->setObjectName("trainingMethod");
    method->setAccessibleName("Training method");
    method->addItems({"LoRA","QLoRA","Full fine-tuning · not available"});
    auto *methods=qobject_cast<QStandardItemModel *>(method->model());
    methods->item(2)->setEnabled(false);
    methods->item(2)->setToolTip("Full fine-tuning is not implemented in the training engine.");
    field(3,0,"Training method",method);
    field(3,1,"Weight precision",m_quantCombo);
    m_qloraCheck=new ToggleSwitch("Use 4-bit QLoRA",m_hyperBox);
    m_qloraCheck->setChecked(false);
    m_qloraCheck->hide(); // Retains compatibility with existing saved presets.
    connect(method,&QComboBox::currentIndexChanged,this,[this](int index) {
        m_qloraCheck->setChecked(index==1);
        if (index==1 && quantizationCode()=="none") m_quantCombo->setCurrentIndex(1);
    });
    connect(m_quantCombo,&QComboBox::currentIndexChanged,this,[this,method] {
        const QSignalBlocker blocker(method);
        method->setCurrentIndex(quantizationCode()=="none" ? 0 : 1);
    });
    connect(m_qloraCheck,&QCheckBox::toggled,this,&TrainingPanel::onQuantizationToggled);
    onQuantizationToggled();
}

void TrainingPanel::setBackendClient(BackendClient *client) {
    m_client = client;
    if (client && client->isReady()) {
        setStatus({});
        refreshOptions();
    } else {
        setStatus("Backend not connected.","warn");
    }
}

void TrainingPanel::refreshOptions() {
    refreshModels();
    refreshDatasets();
    refreshCapabilities();
    refreshJobs();
    refreshTelemetry();
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
    auto *method=findChild<QComboBox *>("trainingMethod");
    if (method) {
        auto *items=qobject_cast<QStandardItemModel *>(method->model());
        items->item(1)->setEnabled(m_quantizationAvailable);
        items->item(1)->setToolTip(m_quantizationDetail);
    }
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
    m_client->get("/models", [this](int status, const QJsonDocument &doc) {
        if (status != 200 || !doc.isArray()) { setStatus("Could not refresh the model list. Your current selection is preserved.","warn",false); return; }
        const auto prev=m_modelCombo->currentText();
        auto *draft=m_configuring?qobject_cast<QComboBox *>(m_dialogEditors.value("baseModel")):nullptr;
        const auto draftText=draft?draft->currentText():QString();
        m_models = doc.array();
        m_modelCombo->blockSignals(true);
        m_modelCombo->clear();
        for (const auto &v : doc.array()) {
            const QJsonObject m = v.toObject();
            m_modelCombo->addItem(m.value("name").toString(), m.value("id").toString());
        }
        m_modelCombo->blockSignals(false);
        applyPreservedText(m_modelCombo, prev);
        if (draft) draft->setCurrentText(draftText);
        updateInputSummary();
    });
}

void TrainingPanel::refreshDatasets(const QString &selectId) {
    if (!m_client || !m_client->isReady()) return;
    m_client->get("/datasets", [this, selectId](int status, const QJsonDocument &doc) {
        if (status != 200 || !doc.isArray()) { setStatus("Could not refresh datasets. Your current selection is preserved.","warn",false); return; }
        const auto prev=m_datasetCombo->currentText();
        auto *draft=m_configuring?qobject_cast<QComboBox *>(m_dialogEditors.value("dataset")):nullptr;
        const auto draftText=draft?draft->currentText():QString();
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
        if (draft) draft->setCurrentText(selectId.isEmpty()?draftText:m_datasetCombo->currentText());
        updateInputSummary();
    });
}

void TrainingPanel::onQuantizationToggled() {
    if (m_qloraCheck->isChecked()) {
        m_quantCombo->setCurrentIndex(m_quantCombo->findText("4-bit (NF4)"));
        m_quantCombo->setEnabled(m_quantizationAvailable);
    } else {
        m_quantCombo->setCurrentIndex(m_quantCombo->findText("None"));
        m_quantCombo->setEnabled(m_quantizationAvailable);
    }
}

void TrainingPanel::onBrowseBaseModel() {
    const QString path = QFileDialog::getExistingDirectory(
        m_configuring?static_cast<QWidget *>(m_runDialog):this, "Select base model directory");
    if (!path.isEmpty()) {
        if (m_configuring) qobject_cast<QComboBox *>(m_dialogEditors["baseModel"])->setCurrentText(path);
        else m_modelCombo->setCurrentText(path);
    }
}

void TrainingPanel::onBrowseDataset() {
    const QString path = QFileDialog::getOpenFileName(
        m_configuring?static_cast<QWidget *>(m_runDialog):this, "Import dataset", QString(),
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
        m_settingsToggle->setChecked(true);
        m_modelCombo->setFocus();
        setStatus("Select or type a base model.", "error");
        return;
    }
    const QString dsText = m_datasetCombo->currentText().trimmed();
    const int di = m_datasetCombo->findText(dsText);
    if (di < 0) {
        m_settingsToggle->setChecked(true);
        m_datasetCombo->setFocus();
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

    const auto previousJob=m_displayedJob;
    m_selectedRunId.clear(); m_cancelPending=false;
    if (!m_configuring) m_settingsToggle->setChecked(false);
    m_workspaceTabs->setCurrentIndex(0);
    m_archiveToggle->setChecked(false);
    if (auto *scroll=findChild<QScrollArea *>("cardScroll")) scroll->verticalScrollBar()->setValue(0);
    ++m_jobsRequest; // Ignore list replies issued before this submission.
    m_startButton->setEnabled(false);
    m_loadPreset->setEnabled(false);
    m_setup->setEnabled(true);
    m_progress->setValue(0);

    m_rawData->clear();
    findChild<QPushButton *>("trainingReviewRun")->setEnabled(false);
    m_displayedJob={}; renderEventLog(); m_runConfiguration->setText("Preparing the run configuration…");
    m_runMeta->setText("Waiting for the backend to accept this run.");
    findChild<QPushButton *>("trainingExportRun")->setEnabled(false);
    findChild<QPushButton *>("trainingReuseRun")->setEnabled(false);
    m_lossChart->setHistory({}); m_lrChart->setHistory({}); m_gradChart->setHistory({});
    m_observatoryRate->setHistory({}); m_observatoryGradient->setHistory({}); m_throughputChart->setHistory({});
    m_badge->setText("Preparing…"); m_runEyebrow->setText("PREPARING EXPERIMENT");
    m_stepLabel->setText("Step — / —"); m_epochLabel->setText("Waiting for the first step");
    m_checkpointLabel->setText("Waiting for the backend to accept this run.");
    for (auto *value : m_configValues) value->setText("—");
    for (auto *note : m_statNotes) note->setText("Waiting for measured samples");
    for (auto *reading : {m_validationReading,m_throughputReading,m_remainingReading}) reading->setText("—");
    for (auto *reading : {m_lossReading,m_rateReading,m_gradientReading}) reading->setText("—");
    m_metrics->setText("Waiting for measured samples.");
    m_runName->setText(m_adapterNameEdit->text().trimmed().isEmpty() ? "New training run" : m_adapterNameEdit->text().trimmed());
    m_activity->setActivity(StudioCanvas::Training);
    m_activity->setMotionEnabled(!QSettings().value("appearance/studio3ReducedMotion",false).toBool());
    m_progressLabel->setText("Preparing your run…");
    m_startButton->setText("Preparing…");
    setStatus("Submitting training job...");
    m_client->post("/training/start", body,
        [this,previousJob](int status, const QJsonDocument &doc) {
            if (status != 202) {
                m_activity->setActivity(StudioCanvas::Idle);
                m_activity->setMotionEnabled(false);
                m_setup->setEnabled(true);
                m_runs->setEnabled(true);
                m_loadPreset->setEnabled(true);
                if (!previousJob.isEmpty()) { m_selectedRunId=previousJob["id"].toString(); renderJob(previousJob); }
                else { m_badge->setText("Ready to retry"); m_runEyebrow->setText("READY TO TRAIN"); }
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
            m_selectedRunId=m_jobId; closeRunDialog(); setStatus({});
            refreshJobs();
            m_lastMessage.clear();
            m_progress->setValue(0);
            m_progressLabel->setText("Queued");
            m_metrics->setText("Waiting for the first training step.");
            addRunEvent("Training request accepted. Waiting for measured steps.","info",m_jobId,"queued");
            m_stopButton->setEnabled(true);
            m_pollTimer->start();
            onPollStatus();
        });
}

void TrainingPanel::onStop() {
    if (m_jobId.isEmpty() || m_selectedRunId!=m_jobId || !m_client || !m_client->isReady()) return;
    if (!m_stopButton->isEnabled() || m_cancelPending) return;
    m_cancelPending=true; m_stopButton->setEnabled(false); m_stopButton->setText("Stopping…");
    m_badge->setText("Stopping…"); addRunEvent("Stop requested. Waiting for the current step to finish.","warn",m_jobId);
    const auto id=m_jobId;
    m_client->post("/training/cancel/" + id, QJsonObject(),
        [this,id](int status, const QJsonDocument &) {
            if (m_jobId != id) return;
            if (status == 200) {
                setStatus("Cancelling...");
            } else {
                m_cancelPending=false; renderJob(m_displayedJob);
                addRunEvent(QString("Stop request failed (HTTP %1).").arg(status),"error",id);
                setStatus(QString("Cancel request failed (HTTP %1).").arg(status), "error");
            }
        });
}

void TrainingPanel::onPollStatus() {
    if (m_jobId.isEmpty() || m_pollPending) return;
    if (!m_client || !m_client->isReady()) {
        reportPollingStatus("Training status unavailable. Reconnecting; the backend may still be training.","warn");
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
                    finishJob("The backend no longer has this training job. Showing its last recorded measurements.","error");
                    m_badge->setText("Unavailable"); applyStatusTone(m_badge,"error"); m_runEyebrow->setText("LAST RECORDED SIGNALS");
                } else reportPollingStatus(QString("Status request failed (HTTP %1). Retrying; the job may still be running.").arg(status),"warn");
                return;
            }
            const auto job=doc.object();
            displayJob(job);
            const QString st=job["status"].toString();
            const QString message=job["message"].toString();
            if (!message.isEmpty() && message!=m_lastMessage) {
                m_lastMessage=message; addRunEvent(message,st=="error"?"error":st=="done"?"success":st=="cancelled"?"warn":"info",id,"message:"+QString::number(job["current_step"].toInt())+":"+st+":"+message);
            }
            if (st=="done" || st=="error" || st=="cancelled") {
                m_pollTimer->stop();
                if(st!="cancelled")emit runFinished(st=="done");
                finishJob(message,st=="done" ? "ok" : st=="cancelled" ? "warn" : "error");
            } else reportPollingStatus({});
        });
}

void TrainingPanel::displayJob(const QJsonObject &job) {
    bool found=false;
    for (int i=0;i<m_jobs.size();++i) if (m_jobs[i].toObject()["id"]==job["id"]) { m_jobs[i]=job; found=true; break; }
    if (!found) m_jobs.prepend(job);
    if (m_selectedRunId.isEmpty() || m_selectedRunId==job["id"].toString()) {
        m_selectedRunId=job["id"].toString(); renderJob(job);
    }
    refreshExperimentViews(); updateComparison();
}

void TrainingPanel::renderJob(const QJsonObject &job) {
    const bool sameJob=!m_displayedJob.isEmpty() && m_displayedJob["id"]==job["id"];
    m_displayedJob=job; recordJobEvents(job);
    findChild<QPushButton *>("trainingExportRun")->setEnabled(true);
    findChild<QPushButton *>("trainingReuseRun")->setEnabled(true);
    const auto precision=job["config"].toObject()["quantization"].toString("none");
    m_runMeta->setText(job["base_model"].toString()+"  ·  "+(precision=="none"?"LoRA":"QLoRA")+"  ·  "+job["created_at"].toString());
    const auto status=job["status"].toString();
    m_runEyebrow->setText(status=="running" || status=="queued" ? "LIVE EXPERIMENT" : "EXPERIMENT RESULTS");
    m_followActive->setVisible(!m_jobId.isEmpty() && job["id"].toString()!=m_jobId);
    m_timeLabel->setText(status=="done" || status=="cancelled" || status=="error" ? "Duration" : "Time remaining");
    m_badge->setText(status=="running" ? "● Training" : status=="queued" ? "● Queued" : status=="done" ? "✓ Completed" : status=="cancelled" ? "Stopped" : "Failed");
    applyStatusTone(m_badge,status=="running" || status=="done" ? "ok" : status=="error" ? "error" : "warn");
    const bool stopping=m_cancelPending && job["id"].toString()==m_jobId && (status=="running" || status=="queued");
    m_stopButton->setText(stopping?"Stopping…":"Stop run");
    if (stopping) m_badge->setText("Stopping…");
    m_stopButton->setEnabled(!stopping && !m_jobId.isEmpty() && job["id"].toString()==m_jobId && (status=="running" || status=="queued"));
    const auto config=job["config"].toObject();
    auto saved=[](const QJsonValue &value) {
        return value.isNull() || value.isUndefined() ? QString("—") : value.toVariant().toString();
    };
    QStringList configuration{job["base_model"].toString("—"),"Dataset: "+job["dataset_id"].toString("—"),QString()};
    for (const auto &field : {qMakePair(QString("quantization"),QString("Precision")),
            qMakePair(QString("rank"),QString("LoRA rank")),qMakePair(QString("alpha"),QString("LoRA alpha")),
            qMakePair(QString("learning_rate"),QString("Learning rate")),qMakePair(QString("batch_size"),QString("Batch size")),
            qMakePair(QString("epochs"),QString("Epochs")),qMakePair(QString("max_length"),QString("Max sequence"))})
        configuration << field.second+": "+saved(config[field.first]);
    m_runConfiguration->setText(configuration.join('\n'));
    m_configValues["model"]->setText(job["base_model"].toString("—"));
    QString datasetName=job["dataset_id"].toString("—");
    for (const auto &value : m_datasets) if (value.toObject()["id"]==job["dataset_id"]) datasetName=value.toObject()["name"].toString(datasetName);
    m_configValues["dataset"]->setText(datasetName);
    m_configValues["method"]->setText(precision=="none" ? "LoRA" : "QLoRA");
    m_configValues["rank"]->setText(saved(config["rank"])+" / "+saved(config["alpha"]));
    m_configValues["lr"]->setText(saved(config["learning_rate"])); m_configValues["batch"]->setText(saved(config["batch_size"]));
    m_configValues["sequence"]->setText(saved(config["max_length"]));
    m_configValues["validation"]->setText(config["validation_split"].isDouble()?QString::number(config["validation_split"].toDouble()*100,'g',4)+"%":"—");

    auto *review=findChild<QPushButton *>("trainingReviewRun");
    review->setEnabled(!job["id"].toString().isEmpty());
    review->setProperty("jobId",job["id"].toString());
    const auto raw=QString::fromUtf8(QJsonDocument(job).toJson(QJsonDocument::Indented));
    if (m_rawData->toPlainText()!=raw) {
        const auto previousCursor=m_rawData->textCursor();
        const int anchor=previousCursor.anchor(),position=previousCursor.position();
        const int vertical=m_rawData->verticalScrollBar()->value(),horizontal=m_rawData->horizontalScrollBar()->value();
        m_rawData->setPlainText(raw);
        if (sameJob) {
            auto cursor=m_rawData->textCursor(); const int end=qMax(0,m_rawData->document()->characterCount()-1);
            cursor.setPosition(qMin(anchor,end)); cursor.setPosition(qMin(position,end),QTextCursor::KeepAnchor);
            m_rawData->setTextCursor(cursor); m_rawData->verticalScrollBar()->setValue(vertical); m_rawData->horizontalScrollBar()->setValue(horizontal);
        }
    }
    const auto history=job["history"].toArray();
    const int total=job["total_steps"].toInt();
    const int step=job["current_step"].toInt();
    m_lossChart->setHistory(history,total);
    m_lrChart->setHistory(history,total);
    m_gradChart->setHistory(history,total);
    m_observatoryRate->setHistory(history,total); m_observatoryGradient->setHistory(history,total);
    m_runName->setText(job["name"].toString("Training run"));
    const double progress=job["progress"].toDouble();
    m_progress->setValue(std::isfinite(progress) ? qBound(0,qRound(progress),100) : 0);
    m_progressLabel->setText(QString("%1% · %2").arg(m_progress->value()).arg(status));
    m_stepLabel->setText(QString("Step %1 / %2").arg(step).arg(total>0?QString::number(total):"—"));
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
    QJsonValue latestLoss;
    for (const auto &value : history) if (value.toObject()["loss"].isDouble()) latestLoss=value.toObject()["loss"];
    m_lossReading->setText(number(latestLoss));
    m_rateReading->setText(number(rate));
    m_gradientReading->setText(number(gradient));
    m_validationReading->setText(number(validation));
    m_throughputReading->setText("—"); m_remainingReading->setText("—");
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
        if (std::isfinite(remaining)) {
            m_remainingReading->setText(TrainingTime::duration(remaining));
            timing=QString("~%1 left · %2 steps/s")
                .arg(TrainingTime::duration(remaining)).arg(steps/seconds,0,'f',2);
        }
    }
    if (steps>0 && seconds>0) m_throughputReading->setText(QString::number(steps/seconds,'f',2));
    QJsonArray throughput; QJsonObject previous;
    for (const auto &value : history) {
        const auto row=value.toObject();
        if (!row["loss"].isDouble() || !row["elapsed_seconds"].isDouble()) continue;
        const double ds=row["step"].toDouble()-previous["step"].toDouble();
        const double dt=row["elapsed_seconds"].toDouble()-previous["elapsed_seconds"].toDouble();
        if (!previous.isEmpty() && ds>0 && dt>0) throughput.append(QJsonObject{{"step",row["step"]},{"step_rate",ds/dt}});
        previous=row;
    }
    m_throughputChart->setHistory(throughput,total);
    m_epochLabel->setText(QString("%1 / %2 epochs  ·  %3%")
        .arg(number(epoch),saved(config["epochs"])).arg(m_progress->value()));
    if (!first.isEmpty() && first["loss"].toDouble()>0 && latestLoss.isDouble())
        m_statNotes[0]->setText(QString("%1 %2% from first sample").arg(latestLoss.toDouble()<=first["loss"].toDouble()?"↓":"↑")
            .arg(std::abs((latestLoss.toDouble()/first["loss"].toDouble()-1)*100),0,'f',1));
    else m_statNotes[0]->setText("Waiting for first sample");

    m_checkpointLabel->setText(status=="done" && !job["adapter_id"].toString().isEmpty() ? "✓ Adapter saved · ready for Playground" :
        status=="cancelled" ? "Run stopped · recorded signals preserved" : history.isEmpty()?"Waiting for measured training steps":"✓ Measured signals saved locally");
    const auto began=QDateTime::fromString(job["started_at"].toString(),Qt::ISODate);
    const auto ended=QDateTime::fromString(job["finished_at"].toString(),Qt::ISODate);
    if (began.isValid() && (ended.isValid() || status=="running" || status=="queued")) elapsed=qMax(qint64(0),began.secsTo(ended.isValid()?ended:QDateTime::currentDateTimeUtc()));
    m_statNotes[1]->setText(validation.isDouble()?"Held-out samples":config["validation_split"].toDouble()>0?"Awaiting validation":"Validation not enabled");
    m_statNotes[3]->setText(TrainingTime::duration(elapsed)+" elapsed");
    if (status=="done" || status=="cancelled" || status=="error")
        m_remainingReading->setText(TrainingTime::duration(elapsed));
    QStringList activity{QString("● %1\n   %2").arg(status=="done"?"Training completed":status=="cancelled"?"Run stopped":"Latest update",job["message"].toString("Waiting for an update"))};
    if (validation.isDouble()) activity << QString("● Validation measured\n   Loss %1 · Epoch %2").arg(number(validation),number(epoch));
    activity << (began.isValid()?"● Training started\n   "+began.toLocalTime().toString("dd MMM, HH:mm"):"● Waiting for training to start");
    m_activityText->setText(activity.join("\n\n"));
    m_metrics->setText(QString("Step %1 / %2  ·  Epoch %3  ·  %4 elapsed\nLR %5  ·  Gradient %6  ·  %7")
        .arg(step).arg(total>0?QString::number(total):"—").arg(number(epoch))
        .arg(TrainingTime::duration(elapsed)).arg(number(rate),number(gradient),timing));
}

void TrainingPanel::refreshJobs() {
    if (!m_client || !m_client->isReady()) return;
    const int request=++m_jobsRequest;
    m_client->get("/training/jobs",[this,request](int status,const QJsonDocument &doc) {
        if (request!=m_jobsRequest) return;
        if (status!=200 || !doc.isArray()) { setStatus("Could not refresh experiments. Existing measurements are preserved.","warn",false); return; }
        m_jobs=doc.array();
        bool selectedExists=false;
        for (const auto &value : m_jobs) selectedExists |= value.toObject()["id"].toString()==m_selectedRunId;
        if (!selectedExists && m_selectedRunId!=m_jobId) m_selectedRunId.clear();
        if (m_jobs.isEmpty() && m_jobId.isEmpty() && m_startButton->isEnabled()) {
            m_selectedRunId.clear(); m_displayedJob={}; m_rawData->clear();
            m_runName->setText("Your next experiment"); m_runEyebrow->setText("READY TO TRAIN");
            m_runMeta->setText("Choose a model and dataset to begin."); m_badge->setText("Ready"); m_timeLabel->setText("Time remaining");
            for (auto *note : m_statNotes) note->setText("Waiting for measured samples");
            applyStatusTone(m_badge,{});
            m_stepLabel->setText("Step — / —"); m_epochLabel->setText("0%"); m_progress->setValue(0);
            for (auto *chart : {m_lossChart,m_lrChart,m_gradChart,m_throughputChart,m_observatoryRate,m_observatoryGradient}) chart->setHistory({});
            for (auto *reading : {m_lossReading,m_validationReading,m_throughputReading,m_remainingReading}) reading->setText("—");
            for (auto *value : m_configValues) value->setText("—");
            for (const auto &name : {"trainingExportRun","trainingReuseRun","trainingReviewRun"}) findChild<QPushButton *>(name)->setEnabled(false);
            m_stopButton->setEnabled(false); m_followActive->hide(); m_activityText->setText("No training experiments yet.");
            m_checkpointLabel->setText("Your measurements will appear when training starts."); renderEventLog();
        }
        QString active;
        for (const auto &value : m_jobs) {
            const auto job=value.toObject();
            if (active.isEmpty() && (job["status"]=="running" || job["status"]=="queued")) active=job["id"].toString();
        }
        if (m_selectedRunId.isEmpty() && !m_jobs.isEmpty()) {
            m_selectedRunId=active.isEmpty()?m_jobs.first().toObject()["id"].toString():active;
            for (const auto &value : m_jobs) if (value.toObject()["id"].toString()==m_selectedRunId) renderJob(value.toObject());
        }
        refreshExperimentViews(); refreshComparisonOptions();
        // Reattach to a job still running after switching/reopening the page.
        if (m_jobId.isEmpty() && m_startButton->isEnabled() && !active.isEmpty()) {
            m_jobId=active; m_lastMessage.clear();
            m_setup->setEnabled(true);
            m_startButton->setEnabled(false); m_startButton->setText("Training…");
            m_loadPreset->setEnabled(false);
            m_stopButton->setEnabled(true);
            m_activity->setActivity(StudioCanvas::Training);
            m_activity->setMotionEnabled(!QSettings().value("appearance/studio3ReducedMotion",false).toBool());
            m_pollTimer->start(); onPollStatus();
        }
    });
}

void TrainingPanel::refreshComparisonOptions() {
    for (auto *combo : {m_compareA,m_compareB}) {
        const auto selected=combo->currentData().toString();
        const QSignalBlocker blocker(combo);
        combo->clear();
        combo->addItem(combo==m_compareA ? "Choose run A" : "Choose run B",QString());
        for (const auto &value : m_jobs) {
            const auto job=value.toObject();
            combo->addItem(job["name"].toString()+" · "+job["status"].toString()+" · "+job["id"].toString().left(8),job["id"].toString());
        }
        const int index=combo->findData(selected);
        combo->setCurrentIndex(qMax(0,index));
    }
    updateComparison();
}

void TrainingPanel::updateComparison() {
    const int previousScroll=m_comparisonTable->verticalScrollBar()->value();
    const auto first=m_compareA->currentData().toString(), second=m_compareB->currentData().toString();
    QJsonObject a,b;
    for (const auto &value : m_jobs) {
        const auto job=value.toObject();
        if (job["id"].toString()==first) a=job;
        if (job["id"].toString()==second) b=job;
    }
    const bool ready=!a.isEmpty() && !b.isEmpty() && first!=second;
    m_comparisonTable->setRowCount(0);
    const auto ah=ready ? a["history"].toArray() : QJsonArray();
    const auto bh=ready ? b["history"].toArray() : QJsonArray();
    const int total=ready ? qMax(a["total_steps"].toInt(),b["total_steps"].toInt()) : 0;
    m_compareLoss->setComparison(ah,bh,total);
    m_compareRate->setComparison(ah,bh,total);
    m_compareGradient->setComparison(ah,bh,total);
    if (!ready) {
        m_comparisonHint->setText(!first.isEmpty() && first==second ? "Choose two different runs to compare." :
            "Choose two runs above. Their measured curves and settings will appear here.");
        m_compareSummary->setText(m_comparisonHint->text());
        if (!m_displayedJob.isEmpty()) m_lossChart->setHistory(m_displayedJob["history"].toArray(),m_displayedJob["total_steps"].toInt());
        return;
    }
    QString hint="A: orange · B: blue · Solid: training · Dashed: validation. Shared axes use actual optimiser steps.";
    if (a["base_model"]!=b["base_model"] || a["dataset_id"]!=b["dataset_id"])
        hint+=" Different models or datasets: loss values may not be directly comparable.";
    if (ah.isEmpty() || bh.isEmpty()) hint+=" One or both runs have no recorded measurements.";
    hint+=" Charts show the retained history (up to 1,000 entries per run).";
    m_comparisonHint->setText(hint);
    auto text=[](const QJsonValue &value) {
        if (value.isDouble()) return std::isfinite(value.toDouble()) ? QString::number(value.toDouble(),'g',6) : QString("—");
        return value.isNull() || value.isUndefined() ? QString("—") : value.toVariant().toString();
    };
    auto row=[this,&text](const QString &label,const QJsonValue &av,const QJsonValue &bv) {
        const int i=m_comparisonTable->rowCount(); m_comparisonTable->insertRow(i);
        m_comparisonTable->setItem(i,0,new QTableWidgetItem(label));
        m_comparisonTable->setItem(i,1,new QTableWidgetItem(text(av)));
        m_comparisonTable->setItem(i,2,new QTableWidgetItem(text(bv)));
    };
    for (const auto &field : {qMakePair(QString("name"),QString("Run")),qMakePair(QString("status"),QString("Status")),
         qMakePair(QString("base_model"),QString("Base model")),qMakePair(QString("dataset_id"),QString("Dataset")),
         qMakePair(QString("loss"),QString("Reported training loss")),qMakePair(QString("current_step"),QString("Completed steps"))})
        row(field.second,a[field.first],b[field.first]);
    auto latest=[](const QJsonArray &history,const QString &key) {
        QJsonValue result;
        for (const auto &value : history) {
            // operator[] can return a reference into the temporary object on
            // older Qt builds. Keep an owned value beyond this statement.
            const QJsonValue v=value.toObject().value(key);
            if (v.isDouble() && std::isfinite(v.toDouble())) result=v;
        }
        return result;
    };
    row("Latest validation loss",latest(ah,"eval_loss"),latest(bh,"eval_loss"));
    const auto ac=a["config"].toObject(),bc=b["config"].toObject();
    const QList<QPair<QString,QString>> fields{{"quantization","Weight precision"},{"rank","Rank"},{"alpha","Alpha"},
        {"dropout","Dropout"},{"epochs","Epochs"},{"learning_rate","Learning rate"},{"batch_size","Batch size"},
        {"max_length","Maximum length"},{"target_modules","Target modules"},{"warmup_ratio","Warmup ratio"},
        {"validation_split","Validation fraction"},{"seed","Seed"}};
    for (const auto &field : fields) row(field.second,ac[field.first],bc[field.first]);
    m_comparisonTable->resizeRowsToContents();
    m_comparisonTable->verticalScrollBar()->setValue(previousScroll);
    if (m_workspaceTabs->currentIndex()==1 && m_archiveToggle->isChecked() && !m_displayedJob.isEmpty()) {
        const auto baseline=m_displayedJob["id"]==b["id"]?a:b;
        m_compareSummary->setText("Baseline  "+baseline["name"].toString()+"    ·    Orange: selected run    /    Blue: baseline");
        m_lossChart->setComparison(m_displayedJob["history"].toArray(),baseline["history"].toArray(),total);
    }
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
    updateRecipeSummary();
    if (m_estimateTimer) {
        ++m_estimateRequest; m_memoryBar->hide();
        m_budgetSummary->setText("Updating estimate…"); m_estimateLabel->setText("Updating resource estimate…"); m_estimateTimer->start();
    }
}

void TrainingPanel::requestEstimate() {
    if (!m_client || !m_client->isReady()) return;
    const int di=m_datasetCombo->findText(m_datasetCombo->currentText());
    if (di<0 || m_modelCombo->currentText().trimmed().isEmpty()) {
        m_memoryBar->hide(); m_estimateLabel->setText("Select a model and dataset to estimate resources."); m_budgetSummary->setText("Choose model and dataset"); return;
    }
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
        if (status!=200 || !doc.isObject()) { m_estimateLabel->setText("Resource estimate unavailable. Change a setting or refresh to retry."); m_budgetSummary->setText("Estimate unavailable"); return; }
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
            lines << QString("Estimated time: ~%1–%2").arg(TrainingTime::duration(e["duration_low_seconds"].toDouble()),
                                                         TrainingTime::duration(e["duration_high_seconds"].toDouble()));
        else lines << "Estimated time: calibrates after a matching run";
        if (e["steps"].isDouble()) lines << QString("%1 optimiser steps").arg(e["steps"].toInt());
        m_estimateLabel->setText(lines.join("    ·    "));
        m_budgetSummary->setText(high>0 ? "~"+gb(low)+"–"+gb(high)+" estimated" : "Estimate unavailable for this model");
        m_estimateLabel->setToolTip(e["assumptions"].toString()+"\n"+e["detail"].toString());
        if (available>0 && high>0) { m_memoryBar->setValue(qBound(0,qRound(high/available*100),100)); m_memoryBar->show(); }
    });
}

void TrainingPanel::finishJob(const QString &message, const QString &tone) {
    m_cancelPending=false; m_followActive->hide();
    m_activity->setActivity(StudioCanvas::Idle);
    m_activity->setMotionEnabled(false);
    m_setup->setEnabled(true);
    m_runs->setEnabled(true);
    m_loadPreset->setEnabled(true);
    if (m_selectedRunId==m_jobId)
        m_progressLabel->setText(tone == "ok" ? "Training complete" : tone == "warn" ? "Training stopped" : "Training interrupted");
    m_startButton->setText("Start Training");
    m_jobId.clear();
    m_startButton->setEnabled(true);
    m_stopButton->setEnabled(false);
    findChild<QPushButton *>("trainingReuseRun")->setEnabled(!m_displayedJob.isEmpty());
    setStatus(message, tone);
    refreshJobs();
    if (tone == "ok")
        emit trainingCompleted();
}

void TrainingPanel::setStatus(const QString &text, const QString &tone, bool dialogFeedback) {
    m_statusLabel->setText(text); m_statusLabel->setVisible(!text.isEmpty());
    if (m_feedbackFrame) m_feedbackFrame->setVisible(!text.isEmpty());
    if (m_runHint) m_runHint->setText(text);
    if (m_dialogStatus && dialogFeedback) {
        m_dialogStatus->setText(text); applyStatusTone(m_dialogStatus,tone);
        for (auto *button : m_runDialog->findChildren<QPushButton *>())
            if (button->property("dialogStart").toBool()) button->setEnabled(m_startButton->isEnabled());
    }
    applyStatusTone(m_statusLabel, tone);
    if (m_dialogStatus) for (auto *button : m_runDialog->findChildren<QPushButton *>())
        if (button->property("dialogStart").toBool()) button->setEnabled(m_startButton->isEnabled());
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

void TrainingPanel::resizeEvent(QResizeEvent *event) {
    BasePanel::resizeEvent(event);
    updateResponsiveLayout();
}

void TrainingPanel::updateResponsiveLayout() {
    if (!m_workspaceGrid) return;
    const bool observatory=m_workspaceTabs->currentIndex()==1;
    const bool compact=width()<(observatory?1180:1000);
    m_workspaceGrid->removeWidget(m_indexCard); m_workspaceGrid->removeWidget(m_center); m_workspaceGrid->removeWidget(m_rail);
    m_indexCard->setVisible(observatory);
    for (int col=0;col<3;++col) { m_workspaceGrid->setColumnStretch(col,0); m_workspaceGrid->setColumnMinimumWidth(col,0); }
    m_rail->setMinimumWidth(0); m_rail->setMaximumWidth(QWIDGETSIZE_MAX);
    if (observatory && !compact) {
        m_indexCard->setFixedWidth(240); m_workspaceGrid->addWidget(m_indexCard,0,0,2,1);
        m_workspaceGrid->addWidget(m_center,0,1); m_workspaceGrid->addWidget(m_rail,0,2); m_rail->setFixedWidth(245);
        m_workspaceGrid->setColumnStretch(1,1);
    } else if (observatory) {
        m_indexCard->setFixedWidth(200); m_workspaceGrid->addWidget(m_indexCard,0,0,2,1);
        m_workspaceGrid->addWidget(m_center,0,1); m_workspaceGrid->addWidget(m_rail,1,1); m_workspaceGrid->setColumnStretch(1,1);
    } else if (!compact) {
        m_workspaceGrid->addWidget(m_center,0,0); m_workspaceGrid->addWidget(m_rail,0,1); m_rail->setFixedWidth(276);
        m_workspaceGrid->setColumnStretch(0,1);
    } else {
        m_workspaceGrid->addWidget(m_center,0,0); m_workspaceGrid->addWidget(m_rail,1,0); m_workspaceGrid->setColumnStretch(0,1);
    }
    m_railLayout->setDirection(!observatory && compact ? QBoxLayout::LeftToRight : QBoxLayout::TopToBottom);
    m_pageTitle->setText(observatory?"Run observatory":"Training studio");
    m_pageSubtitle->setText(observatory?"Follow the signals. Understand the difference between runs.":"Your next iteration starts with what you learned.");
    m_breadcrumb->setText(observatory?"Workspace   /   Training   /   Observatory":"Workspace   /   Training   /   Studio");
    const auto titles=findChildren<QLabel *>("trainingTitleMarker");
    if (!titles.isEmpty()) { titles.first()->setText(observatory?"OBSERVE / COMPARE":"CREATE / ITERATE"); titles.first()->setVisible(width()>=1100); }
    for (auto *value : {m_lossReading,m_validationReading,m_throughputReading,m_remainingReading}) {
        value->setProperty("compact",observatory); value->style()->unpolish(value); value->style()->polish(value);
    }
    m_chartStack->setFixedHeight(observatory?245:270);
}

void TrainingPanel::setWorkspace(int index) {
    m_archiveToggle->setVisible(index==1);
    if (index==0) m_archiveToggle->setChecked(false);
    m_comparisonPanel->setVisible(index==1 && m_archiveToggle->isChecked());
    m_compareSummary->setVisible(index==1 && m_archiveToggle->isChecked());
    m_observatorySignals->setVisible(index==1 && m_visualise->isChecked());
    m_activityCard->setVisible(index==1);
    m_railLayout->removeWidget(m_computeCard); m_railLayout->removeWidget(m_configCard); m_railLayout->removeWidget(m_activityCard);
    if (index==1) { m_railLayout->insertWidget(0,m_configCard); m_railLayout->insertWidget(1,m_activityCard); m_railLayout->insertWidget(2,m_computeCard); }
    else { m_railLayout->insertWidget(0,m_computeCard); m_railLayout->insertWidget(1,m_configCard); m_railLayout->insertWidget(2,m_activityCard); }
    m_archive->setVisible(index==0 || m_archiveToggle->isChecked());
    findChild<QPushButton *>("trainingCompareAction")->setVisible(index==1);
    setProperty("observatory",index==1); style()->unpolish(this); style()->polish(this);
    m_setup->setVisible(index==0 || m_configuring);
    updateResponsiveLayout();
}

void TrainingPanel::filterRuns() {
    const auto *search=findChild<QLineEdit *>("trainingRunSearch"); if (!search || !m_runs || !m_experiments) return;
    int visible=0;
    for (int i=0;i<m_runs->count();++i) {
        const bool hidden=!m_runs->item(i)->data(Qt::AccessibleTextRole).toString().contains(search->text(),Qt::CaseInsensitive);
        m_runs->item(i)->setHidden(hidden); if (!hidden) ++visible; if (i<m_experiments->rowCount()) m_experiments->setRowHidden(i,hidden);
    }
    const auto message=m_jobs.isEmpty()?QString("No experiments yet. Choose New run to train your first adapter."):QString("No experiments match “%1”. Clear the search to see all runs.").arg(search->text());
    m_historyEmpty->setText(message); m_indexEmpty->setText(message);
    m_historyEmpty->setVisible(visible==0); m_indexEmpty->setVisible(visible==0);
    m_experiments->setVisible(visible>0); m_runs->setVisible(visible>0);
}

void TrainingPanel::reuseRun() {
    if (m_displayedJob.isEmpty()) return;
    const auto c=m_displayedJob["config"].toObject();
    auto preset=QJsonDocument::fromJson(settingsJson().toUtf8()).object();
    // Only replace settings actually recorded in this run; older jobs may lack fields.
    const QList<QPair<QString,QString>> fields{{"rank","rank"},{"alpha","alpha"},{"dropout","dropout"},
        {"epochs","epochs"},{"batch_size","batchSize"},{"learning_rate","learningRate"},
        {"max_length","maxLength"},{"target_modules","targetModules"},{"warmup_ratio","warmup"}};
    for (const auto &field : fields) if (c.contains(field.first)) preset[field.second]=c[field.first];
    if (c["validation_split"].isDouble()) preset["validationSplit"]=c["validation_split"].toDouble()*100;
    preset["baseModel"]=m_displayedJob["base_model"];
    const auto dataset=m_displayedJob["dataset_id"].toString();
    const int index=m_datasetCombo->findData(dataset);
    preset["dataset"]=index>=0 ? m_datasetCombo->itemText(index) : dataset;
    preset["adapterName"]=m_displayedJob["name"].toString()+"-next";
    if (c.contains("quantization")) {
        const auto precision=c["quantization"].toString();
        preset["qlora"]=precision!="none";
        preset["quantization"]=precision=="4bit" ? "4-bit (NF4)" : precision=="8bit" ? "8-bit (bitsandbytes)" : "None";
    }
    openRunDialog(); populateDialogRecipe(preset);
    setStatus("Review this draft, then save the recipe or start training.");
}

void TrainingPanel::refreshExperimentViews() {
    if (!m_experiments) return;
    const int scroll=m_runs->verticalScrollBar()->value();
    const int tableScroll=m_experiments->verticalScrollBar()->value();
    const QSignalBlocker indexBlocker(m_runs),tableBlocker(m_experiments);
    m_runs->clear(); m_experiments->setRowCount(m_jobs.size());
    auto number=[](QJsonValue v) { return v.isDouble() && std::isfinite(v.toDouble()) ? QString::number(v.toDouble(),'f',3) : QString("—"); };
    for (int i=0;i<m_jobs.size();++i) {
        const auto job=m_jobs[i].toObject(); const auto id=job["id"].toString(); const auto status=job["status"].toString();
        const QString result=status=="done"?"Completed":status=="running"?"Training":status=="queued"?"Queued":status=="cancelled"?"Stopped":"Failed";
        QJsonValue validation;
        for (const auto &value : job["history"].toArray()) if (value.toObject()["eval_loss"].isDouble()) validation=value.toObject()["eval_loss"];
        auto *item=new QListWidgetItem(m_runs); item->setData(Qt::UserRole,id);
        item->setData(Qt::AccessibleTextRole,job["name"].toString()+", "+job["base_model"].toString()+", "+result);
        item->setToolTip(job["name"].toString()+"\n"+job["base_model"].toString()+" · "+result);
        auto *row=new QWidget(m_runs); row->setObjectName("trainingIndexItem");
        auto *body=new QVBoxLayout(row); body->setContentsMargins(11,12,11,12); body->setSpacing(5);
        auto *name=new QLabel(job["name"].toString(),row); name->setTextFormat(Qt::PlainText); name->setObjectName("trainingIndexName");
        name->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred); body->addWidget(name);
        auto *model=new QLabel(job["base_model"].toString(),row); model->setTextFormat(Qt::PlainText); model->setObjectName("trainingMicro");
        model->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred); body->addWidget(model);
        auto *resultRow=new QHBoxLayout(); auto *loss=new QLabel(number(job["loss"]),row); loss->setObjectName("trainingIndexLoss");
        resultRow->addWidget(loss); resultRow->addStretch(); auto *state=new QLabel(result,row); state->setObjectName("trainingTiny"); resultRow->addWidget(state); body->addLayout(resultRow);
        row->setAttribute(Qt::WA_TransparentForMouseEvents); item->setSizeHint(QSize(0,93)); m_runs->setItemWidget(item,row);
        const QStringList cells{job["name"].toString()+"\n"+job["created_at"].toString(),job["base_model"].toString(),number(job["loss"]),number(validation),result,"↗"};
        for (int col=0;col<cells.size();++col) {
            auto *cell=new QTableWidgetItem(col==0?QString():cells[col]); cell->setData(Qt::UserRole,id); cell->setToolTip(col==5?"Use this experiment as the next recipe":cells[col]);
            QFont cellFont(Theme::bodyFont()); cellFont.setPixelSize(10); cell->setFont(cellFont); m_experiments->setItem(i,col,cell);
            if (col==0) {
                cell->setData(Qt::AccessibleTextRole,cells[col]);
                auto *nameCell=new QWidget(this); nameCell->setAttribute(Qt::WA_TransparentForMouseEvents);
                auto *nameBody=new QVBoxLayout(nameCell); nameBody->setContentsMargins(15,8,12,8); nameBody->setSpacing(3);
                auto *nameLabel=new QLabel(job["name"].toString(),nameCell); nameLabel->setObjectName("trainingIndexName"); nameLabel->setTextFormat(Qt::PlainText);
                auto *dateLabel=new QLabel(job["created_at"].toString(),nameCell); dateLabel->setObjectName("trainingMicro"); dateLabel->setTextFormat(Qt::PlainText);
                nameBody->addWidget(nameLabel); nameBody->addWidget(dateLabel); m_experiments->setCellWidget(i,col,nameCell);
            }
            if (col==4) cell->setForeground(QColor(status=="error"?Theme::statusError():status=="done"||status=="running"?Theme::statusSuccess():Theme::textMuted()));
        }
        m_experiments->setRowHeight(i,52);
        if (id==m_selectedRunId) { m_runs->setCurrentItem(item); m_experiments->selectRow(i); }
    }
    m_runs->verticalScrollBar()->setValue(scroll); m_experiments->verticalScrollBar()->setValue(tableScroll);
    m_experiments->setFixedHeight(qBound(100,34+m_jobs.size()*52,350)); filterRuns();
}

void TrainingPanel::selectRun(const QString &id) {
    for (const auto &value : m_jobs) if (value.toObject()["id"].toString()==id) {
        m_selectedRunId=id; renderJob(value.toObject());
        const QSignalBlocker listBlocker(m_runs),tableBlocker(m_experiments);
        for (int i=0;i<m_runs->count();++i) if (m_runs->item(i)->data(Qt::UserRole).toString()==id) {
            m_runs->setCurrentRow(i); m_experiments->selectRow(i); break;
        }
        updateComparison(); return;
    }
}

void TrainingPanel::refreshTelemetry() {
    if (m_telemetryPending || !m_client || !m_client->isReady()) return;
    m_telemetryPending=true;
    m_client->get("/training/telemetry",[this](int status,const QJsonDocument &doc) {
        m_telemetryPending=false;
        const auto data=status==200 && doc.isObject()?doc.object():QJsonObject();
        auto number=[](QJsonValue value,int places=0) {
            return value.isDouble() && std::isfinite(value.toDouble()) ? QString::number(value.toDouble(),'f',places) : QString("—");
        };
        auto gb=[&number](QJsonValue bytes) { return bytes.isDouble()?number(bytes.toDouble()/(1024.0*1024*1024),1)+" GB":QString("—"); };
        m_computeValues["accelerator"]->setText(data["accelerator"].toString("LOCAL"));
        m_computeValues["device"]->setText(data["device"].toString("Device telemetry unavailable"));
        m_computeValues["capacity"]->setText(gb(data["total_bytes"])+" "+data["kind"].toString("memory"));
        m_computeValues["utilization"]->setText(data["utilization_percent"].isDouble()?number(data["utilization_percent"])+"%":"—");
        m_computeValues["memory"]->setText(gb(data["used_bytes"])+" / "+gb(data["total_bytes"]));
        const double used=data["used_bytes"].toDouble(),total=data["total_bytes"].toDouble();
        auto *memory=findChild<QProgressBar *>("trainingDeviceMemory");
        memory->setValue(total>0?qBound(0,qRound(used/total*100),100):0);
        m_computeValues["memoryDetail"]->setText(data["used_bytes"].isDouble()&&total>0?"Device-wide usage       Free "+gb(qMax(0.0,total-used)):"Device-wide memory unavailable");
        m_computeValues["temperature"]->setText("Temperature   "+(data["temperature_c"].isDouble()?number(data["temperature_c"])+"°C":"—"));
        m_computeValues["power"]->setText("Power   "+(data["power_w"].isDouble()?number(data["power_w"])+" W":"—"));
        m_computeCard->setToolTip(data["detail"].toString("This backend does not expose device telemetry."));
        const int active=data["utilization_percent"].isDouble()?qRound(data["utilization_percent"].toDouble()/100*32):0;
        for (int i=0;i<m_utilSegments.size();++i) {
            auto *bar=m_utilSegments[i]; const bool lit=i<active;
            if (bar->property("lit").toBool()!=lit) { bar->setProperty("lit",lit); bar->style()->unpolish(bar); bar->style()->polish(bar); }
        }
    });
}

void TrainingPanel::openRunDialog() {
    if (m_configuring) { m_runDialog->raise(); return; }
    m_configuring=true; ++m_dialogEstimateGeneration;
    const auto screen=m_runDialog->screen(); const auto available=screen?screen->availableGeometry().size():QSize(1024,768);
    const bool shortScreen=available.height()<600;
    m_dialogLayout->setContentsMargins(shortScreen?6:12,shortScreen?6:12,shortScreen?6:12,shortScreen?6:12);
    m_dialogEditors.clear(); m_dialogStatus=nullptr; m_dialogEstimate=nullptr;
    while (auto *item=m_dialogLayout->takeAt(0)) { if (item->widget()) delete item->widget(); delete item; }
    auto *container=new QWidget(m_runDialog); auto *shell=new QVBoxLayout(container); shell->setContentsMargins(shortScreen?8:12,shortScreen?8:12,shortScreen?8:12,shortScreen?8:12); shell->setSpacing(shortScreen?10:14);
    auto *scroll=new QScrollArea(container); scroll->setObjectName("trainingDialogScroll"); scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true); scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); scroll->setMinimumHeight(shortScreen?32:180);
    auto *body=new QWidget(scroll); auto *form=new QVBoxLayout(body); form->setContentsMargins(4,2,12,8); form->setSpacing(18);
    auto text=[body](const QString &value,const char *name) {
        auto *label=new QLabel(value,body); label->setObjectName(name); label->setWordWrap(true); label->setTextFormat(Qt::PlainText); return label;
    };
    auto *dialogHeading=new QHBoxLayout(); auto *dialogTitles=new QVBoxLayout(); dialogTitles->setSpacing(6);
    dialogTitles->addWidget(text("RUN CONFIGURATION","trainingEyebrow")); dialogTitles->addWidget(text("Configure your next run","trainingDialogTitle"));
    dialogHeading->addLayout(dialogTitles,1); auto *close=new Button("×",container); close->setFixedWidth(32); close->setFlat(true);
    close->setAccessibleName("Close run configuration"); close->setToolTip("Close · Esc"); dialogHeading->addWidget(close,0,Qt::AlignTop);
    connect(close,&QPushButton::clicked,m_runDialog,&QDialog::reject); shell->addLayout(dialogHeading);
    if (!shortScreen) shell->addWidget(text("Choose your inputs and training settings in one place.","trainingMicro"));
    auto *name=new QLineEdit(m_adapterNameEdit->text(),body); name->setObjectName("trainingDraft_adapterName"); name->setAccessibleName("Adapter name");
    name->setPlaceholderText("Auto-named when empty"); name->setMaxLength(160); m_dialogEditors["adapterName"]=name;
    auto *nameField=new QVBoxLayout(); nameField->setSpacing(7); auto *nameLabel=text("Adapter name","trainingMicro"); nameLabel->setBuddy(name);
    nameField->addWidget(nameLabel); nameField->addWidget(name); form->addLayout(nameField);
    auto *inputs=new QHBoxLayout(); inputs->setSpacing(12);
    if (available.width()<640) inputs->setDirection(QBoxLayout::TopToBottom);
    auto combo=[this,body,text](const QString &key,const QString &title,QComboBox *source) {
        auto *editor=new ComboBox(body); editor->setModel(source->model()); editor->setEditable(source->isEditable());
        editor->setMinimumContentsLength(5); editor->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        editor->setInsertPolicy(QComboBox::NoInsert); editor->setCurrentIndex(source->currentIndex()); editor->setCurrentText(source->currentText());
        if (editor->lineEdit()) editor->lineEdit()->setCursorPosition(0);
        editor->setObjectName("trainingDraft_"+key); editor->setAccessibleName(title); m_dialogEditors[key]=editor;
        auto *caption=text(title,"trainingMicro"); caption->setBuddy(editor);
        return qMakePair(caption,editor);
    };
    for (const auto &field : {qMakePair(QString("baseModel"),QString("01 / BASE MODEL")),qMakePair(QString("dataset"),QString("02 / DATASET")),qMakePair(QString("method"),QString("03 / METHOD"))}) {
        auto *source=field.first=="baseModel"?m_modelCombo:field.first=="dataset"?m_datasetCombo:findChild<QComboBox *>("trainingMethod");
        const auto widgets=combo(field.first,field.second,source); auto *column=new QVBoxLayout(); column->setSpacing(7);
        column->addWidget(widgets.first); column->addWidget(widgets.second); inputs->addLayout(column,field.first=="method"?1:2);
    }
    form->addLayout(inputs);
    auto *inputActions=new QHBoxLayout(); auto *folder=new Button("Model folder…",body); folder->setFlat(true); folder->setObjectName("trainingDraftFolder");
    auto *importDataset=new Button("Import dataset…",body); importDataset->setFlat(true); importDataset->setObjectName("trainingDraftImport");
    inputActions->addWidget(folder); inputActions->addWidget(importDataset); inputActions->addStretch(); form->addLayout(inputActions);
    connect(folder,&QPushButton::clicked,this,&TrainingPanel::onBrowseBaseModel); connect(importDataset,&QPushButton::clicked,this,&TrainingPanel::onBrowseDataset);
    auto *fields=new QGridLayout(); fields->setHorizontalSpacing(16); fields->setVerticalSpacing(7);
    auto integer=[this,body,text](QGridLayout *grid,int row,int col,const QString &key,const QString &title,QSpinBox *source) {
        auto *editor=new SpinBox(body); editor->setRange(source->minimum(),source->maximum()); editor->setValue(source->value());
        editor->setObjectName("trainingDraft_"+key); editor->setAccessibleName(title); m_dialogEditors[key]=editor;
        auto *caption=text(title,"trainingMicro"); caption->setBuddy(editor);
        grid->addWidget(caption,row*3,col); grid->addWidget(editor,row*3+1,col); grid->setRowMinimumHeight(row*3+2,9); grid->setColumnStretch(col,1);
    };
    auto decimal=[this,body,text](QGridLayout *grid,int row,int col,const QString &key,const QString &title,QDoubleSpinBox *source) {
        auto *editor=new DoubleSpinBox(body); editor->setDecimals(source->decimals()); editor->setRange(source->minimum(),source->maximum());
        editor->setSingleStep(source->singleStep()); editor->setSuffix(source->suffix()); editor->setValue(source->value());
        editor->setObjectName("trainingDraft_"+key); editor->setAccessibleName(title); m_dialogEditors[key]=editor;
        auto *caption=text(title,"trainingMicro"); caption->setBuddy(editor);
        grid->addWidget(caption,row*3,col); grid->addWidget(editor,row*3+1,col); grid->setRowMinimumHeight(row*3+2,9); grid->setColumnStretch(col,1);
    };
    integer(fields,0,0,"epochs","Epochs",m_epochsSpin); decimal(fields,0,1,"learningRate","Learning rate",m_lrSpin);
    integer(fields,1,0,"rank","LoRA rank",m_rankSpin); integer(fields,1,1,"batchSize","Batch size",m_batchSpin);
    decimal(fields,2,0,"validationSplit","Validation holdout (%)",m_validationSpin); form->addLayout(fields);
    auto *advancedToggle=new Button("Advanced settings ▾",body); advancedToggle->setObjectName("trainingDraftAdvanced"); advancedToggle->setCheckable(true); advancedToggle->setFlat(true);
    form->addWidget(advancedToggle,0,Qt::AlignLeft);
    auto *advanced=new QWidget(body); auto *advancedGrid=new QGridLayout(advanced); advancedGrid->setContentsMargins(0,0,0,0); advancedGrid->setHorizontalSpacing(16); advancedGrid->setVerticalSpacing(7);
    integer(advancedGrid,0,0,"alpha","LoRA alpha",m_alphaSpin); decimal(advancedGrid,0,1,"dropout","Dropout",m_dropoutSpin);
    integer(advancedGrid,1,0,"maxLength","Maximum sequence",m_maxLengthSpin); decimal(advancedGrid,1,1,"warmup","Warmup ratio",m_warmupSpin);
    const auto modules=combo("targetModules","Target modules",m_targetModules),precision=combo("quantization","Weight precision",m_quantCombo);
    advancedGrid->addWidget(modules.first,6,0); advancedGrid->addWidget(modules.second,7,0);
    advancedGrid->addWidget(precision.first,6,1); advancedGrid->addWidget(precision.second,7,1);
    precision.second->setEnabled(m_quantizationAvailable); precision.second->setToolTip(m_quantizationDetail);
    auto *method=qobject_cast<QComboBox *>(m_dialogEditors["method"]);
    connect(method,&QComboBox::currentIndexChanged,this,[this,precision](int value) {
        const QSignalBlocker blocker(precision.second);
        if (value==0) precision.second->setCurrentIndex(0);
        else if (value==1 && precision.second->currentIndex()==0) precision.second->setCurrentIndex(1);
        queueDialogEstimate();
    });
    connect(precision.second,&QComboBox::currentIndexChanged,this,[this,method](int value) {
        const QSignalBlocker blocker(method); method->setCurrentIndex(value==0?0:1); queueDialogEstimate();
    });
    form->addWidget(advanced); advanced->hide(); connect(advancedToggle,&QPushButton::toggled,advanced,&QWidget::setVisible);
    m_dialogEstimate=text("Calculating resources for this recipe…","trainingDialogEstimate"); form->addWidget(m_dialogEstimate);
    scroll->setWidget(body); shell->addWidget(scroll,1);
    m_dialogStatus=text(m_jobId.isEmpty()?"":"A run is active. You can save this recipe while it finishes.","statusLabel");
    m_dialogStatus->setObjectName("trainingDialogStatus"); shell->addWidget(m_dialogStatus);
    auto *buttons=new QHBoxLayout(); auto *save=new Button("Save recipe",container); save->setObjectName("trainingDraftSave"); save->setFlat(true);
    buttons->addWidget(save); buttons->addStretch();
    auto *start=new Button("Start local training ↗",container); start->setObjectName("primaryBtn"); start->setProperty("dialogStart",true);
    start->setToolTip("Start local training · Ctrl+Enter"); start->setEnabled(m_startButton->isEnabled()); buttons->addWidget(start); shell->addLayout(buttons);
    connect(save,&QPushButton::clicked,this,[this] { if (!validateDialog()) return; applyDialogRecipe(); m_runDialog->accept(); setStatus("Next run recipe saved.","ok"); });
    connect(start,&QPushButton::clicked,this,[this,start] { if (!validateDialog()) return; applyDialogRecipe(); onStart(); start->setEnabled(m_startButton->isEnabled()); });
    auto *shortcut=new QShortcut(QKeySequence(Qt::CTRL|Qt::Key_Return),container); shortcut->setContext(Qt::WidgetWithChildrenShortcut);
    connect(shortcut,&QShortcut::activated,start,&QPushButton::click);
    for (auto *button : container->findChildren<QPushButton *>()) { button->setAutoDefault(false); button->setDefault(false); }
    for (auto *editor : m_dialogEditors) {
        if (auto *box=qobject_cast<QComboBox *>(editor)) connect(box,&QComboBox::currentTextChanged,this,&TrainingPanel::queueDialogEstimate);
        if (auto *spin=qobject_cast<QSpinBox *>(editor)) connect(spin,&QSpinBox::valueChanged,this,&TrainingPanel::queueDialogEstimate);
        if (auto *spin=qobject_cast<QDoubleSpinBox *>(editor)) connect(spin,&QDoubleSpinBox::valueChanged,this,&TrainingPanel::queueDialogEstimate);
    }
    m_dialogLayout->addWidget(container);
    m_runDialog->setMaximumSize(qMax(360,available.width()-40),qMax(360,available.height()-60));
    m_runDialog->resize(qMin(700,available.width()-40),qMin(720,available.height()-60));
    wireScrolling(container); m_runDialog->show(); m_runDialog->raise(); m_runDialog->activateWindow(); name->setFocus(); queueDialogEstimate();
}

QJsonObject TrainingPanel::dialogRecipe() const {
    auto preset=QJsonDocument::fromJson(settingsJson().toUtf8()).object();
    for (auto it=m_dialogEditors.begin();it!=m_dialogEditors.end();++it) {
        if (it.key()=="method") continue;
        if (auto *edit=qobject_cast<QLineEdit *>(it.value())) preset[it.key()]=edit->text().trimmed();
        else if (auto *combo=qobject_cast<QComboBox *>(it.value())) preset[it.key()]=combo->currentText().trimmed();
        else if (auto *spin=qobject_cast<QSpinBox *>(it.value())) preset[it.key()]=spin->value();
        else if (auto *spin=qobject_cast<QDoubleSpinBox *>(it.value())) preset[it.key()]=spin->value();
    }
    preset["qlora"]=preset["quantization"].toString()!="None";
    return preset;
}

void TrainingPanel::applyDialogRecipe() {
    for (auto *editor : m_dialogEditors) if (auto *spin=qobject_cast<QAbstractSpinBox *>(editor)) spin->interpretText();
    applySettingsJson(QString::fromUtf8(QJsonDocument(dialogRecipe()).toJson()));
}

void TrainingPanel::populateDialogRecipe(const QJsonObject &recipe) {
    for (auto it=m_dialogEditors.begin();it!=m_dialogEditors.end();++it) {
        if (!recipe.contains(it.key())) continue;
        const QSignalBlocker blocker(it.value()); const auto value=recipe[it.key()];
        if (auto *edit=qobject_cast<QLineEdit *>(it.value())) edit->setText(value.toString());
        else if (auto *combo=qobject_cast<QComboBox *>(it.value())) { combo->setCurrentText(value.toString()); if (combo->lineEdit()) combo->lineEdit()->setCursorPosition(0); }
        else if (auto *spin=qobject_cast<QSpinBox *>(it.value())) spin->setValue(value.toInt());
        else if (auto *spin=qobject_cast<QDoubleSpinBox *>(it.value())) spin->setValue(value.toDouble());
    }
    const auto precision=qobject_cast<QComboBox *>(m_dialogEditors["quantization"])->currentIndex();
    qobject_cast<QComboBox *>(m_dialogEditors["method"])->setCurrentIndex(precision==0?0:1); queueDialogEstimate();
}

bool TrainingPanel::validateDialog() {
    for (auto *editor : m_dialogEditors) if (auto *spin=qobject_cast<QAbstractSpinBox *>(editor)) spin->interpretText();
    auto invalid=[this](const QString &key,const QString &message) {
        auto *editor=m_dialogEditors.value(key); editor->setProperty("invalid",true); editor->style()->unpolish(editor); editor->style()->polish(editor);
        editor->setFocus(); m_dialogStatus->setText(message); applyStatusTone(m_dialogStatus,"error");
        if (auto *scroll=m_runDialog->findChild<QScrollArea *>("trainingDialogScroll")) scroll->ensureWidgetVisible(editor,0,16);
        return false;
    };
    const auto recipe=dialogRecipe();
    if (recipe["baseModel"].toString().isEmpty()) return invalid("baseModel","Choose a base model or enter its model ID.");
    if (m_datasetCombo->findText(recipe["dataset"].toString())<0) return invalid("dataset","Choose an imported dataset, or use Import dataset to add one.");
    if (!m_quantizationAvailable && recipe["quantization"].toString()!="None") return invalid("quantization",m_quantizationDetail);
    return true;
}

void TrainingPanel::queueDialogEstimate() {
    if (!m_configuring || !m_dialogEstimate) return;
    ++m_dialogEstimateGeneration; m_dialogEstimate->setText("Calculating resources for this recipe…");
    if (m_dialogStatus && m_dialogStatus->property("state")=="error") {
        m_dialogStatus->setText(m_jobId.isEmpty()?"":"A run is active. You can save this recipe while it finishes."); applyStatusTone(m_dialogStatus,{});
    }
    for (auto *editor : m_dialogEditors) if (editor->property("invalid").toBool()) {
        editor->setProperty("invalid",false); editor->style()->unpolish(editor); editor->style()->polish(editor);
    }
    m_dialogEstimateTimer->start();
}

void TrainingPanel::requestDialogEstimate() {
    if (!m_configuring || !m_dialogEstimate) return;
    const auto recipe=dialogRecipe(); const auto base=recipe["baseModel"].toString(),dataset=recipe["dataset"].toString();
    const int mi=m_modelCombo->findText(base),di=m_datasetCombo->findText(dataset);
    if (base.isEmpty() || di<0) { m_dialogEstimate->setText("Choose a model and an imported dataset to estimate resources."); return; }
    if (!m_client || !m_client->isReady()) { m_dialogEstimate->setText("Connect to the local backend to estimate resources."); return; }
    const auto precision=recipe["quantization"].toString();
    const QJsonObject body{{"base_model",mi>=0?m_modelCombo->itemData(mi).toString():base},{"dataset_id",m_datasetCombo->itemData(di).toString()},
        {"rank",recipe["rank"]},{"epochs",recipe["epochs"]},{"batch_size",recipe["batchSize"]},{"max_length",recipe["maxLength"]},
        {"target_modules",recipe["targetModules"]},{"quantization",precision.startsWith("4-bit")?"4bit":precision.startsWith("8-bit")?"8bit":"none"},
        {"validation_split",recipe["validationSplit"].toDouble()/100}};
    const int generation=m_dialogEstimateGeneration;
    m_client->post("/training/estimate",body,[this,generation](int status,const QJsonDocument &doc) {
        if (!m_configuring || generation!=m_dialogEstimateGeneration || !m_dialogEstimate) return;
        if (status!=200 || !doc.isObject()) { m_dialogEstimate->setText("Resource estimate unavailable. Change a setting to retry. Training can still be started."); return; }
        const auto estimate=doc.object(); const auto memory=estimate["memory"].toObject();
        const double low=estimate["memory_low_bytes"].toDouble(),high=estimate["memory_high_bytes"].toDouble(),available=memory["available_bytes"].toDouble();
        auto gb=[](double bytes) { return QString::number(bytes/(1024.0*1024*1024),'f',1)+" GB"; };
        QString summary=high>0 ? (qFuzzyCompare(low,high)?"~"+gb(high):"~"+gb(low)+"–"+gb(high))+" estimated" : "Memory estimate unavailable for this model";
        if (available>0) summary+=" · "+gb(available)+" "+memory["kind"].toString()+" available";
        if (available>0 && high>available) summary+="\nMay exceed available memory. Try a smaller batch or shorter sequence.";
        summary+="\nAdvisory estimate; actual usage depends on the training workload.";
        m_dialogEstimate->setText(summary); m_dialogEstimate->setToolTip(estimate["assumptions"].toString()+"\n"+estimate["detail"].toString());
    });
}

void TrainingPanel::closeRunDialog() {
    if (!m_configuring) return;
    m_configuring=false; ++m_dialogEstimateGeneration; m_dialogEstimateTimer->stop();
    m_runDialog->hide(); m_dialogStatus=nullptr; m_dialogEstimate=nullptr; updateRecipeSummary();
}

void TrainingPanel::updateRecipeSummary() {
    if (!m_recipeSummary) return;
    m_recipeSummary->setText(m_modelCombo->currentText()+"   →   "+m_datasetCombo->currentText()+"   →   "+
        (quantizationCode()=="none"?"LoRA":"QLoRA")+"  RANK "+QString::number(m_rankSpin->value()));
    m_recipeFacts->setText(QString("%1 epochs   ·   LR %2   ·   Batch %3   ·   %4% validation")
        .arg(m_epochsSpin->value()).arg(m_lrSpin->value(),0,'g',4).arg(m_batchSpin->value()).arg(m_validationSpin->value(),0,'f',0));
}

void TrainingPanel::reportPollingStatus(const QString &message,const QString &tone) {
    if (!tone.isEmpty()) { setStatus(message,tone,false); m_runHealthWarning=true; m_runHealthMessage=message; }
    else if (m_runHealthWarning) {
        m_runHealthWarning=false;
        if (m_statusLabel->text()==m_runHealthMessage) setStatus({}, {},false);
        m_runHealthMessage.clear();
    }
}

void TrainingPanel::changeEvent(QEvent *event) {
    BasePanel::changeEvent(event);
    if ((event->type()==QEvent::StyleChange || event->type()==QEvent::PaletteChange) && !m_appearanceRefreshPending) {
        m_appearanceRefreshPending=true;
        QTimer::singleShot(0,this,[this] { m_appearanceRefreshPending=false; refreshRunAppearance(); });
    }
}

void TrainingPanel::refreshRunAppearance() {
    if (!m_legend) return;
    if (m_log) for (auto *highlighter : m_log->document()->findChildren<QSyntaxHighlighter *>()) highlighter->rehighlight();
    const bool loss=m_metricTabs->currentIndex()==0;
    m_legend->setText(loss?QString("<span style='color:%1'>━</span> Training &nbsp; <span style='color:%2'>┄</span> Validation").arg(Theme::accentPrimary(),Theme::statusSuccess()):QString("<span style='color:%1'>━</span> Measured values").arg(Theme::accentPrimary()));
    for (auto *icon : findChildren<QLabel *>()) {
        const auto name=icon->property("trainingIcon").toString(); if (name.isEmpty()) continue;
        icon->setPixmap(Theme::iconPixmap(name,QColor(name=="layers"?Theme::accentLabel():name=="database"?Theme::textMuted():Theme::textSecondary()),name=="database"?14:name=="layers"?18:16,devicePixelRatioF()));
    }
    if (auto *button=findChild<QPushButton *>("trainingExportRun")) button->setIcon(Theme::coloredIcon("file-up",QColor(Theme::textSecondary()),14));
}

void TrainingPanel::wireScrolling(QWidget *root) {
    for (auto *area : root->findChildren<QAbstractScrollArea *>()) {
        area->installEventFilter(this); area->viewport()->installEventFilter(this);
        if (area->objectName()=="cardScroll" || area->property("transientScroll").toBool()) continue;
        area->setProperty("transientScroll",true);
        auto *idle=new QTimer(area); idle->setObjectName("scrollIdleTimer"); idle->setSingleShot(true); idle->setInterval(850);
        for (auto *bar : {area->verticalScrollBar(),area->horizontalScrollBar()}) {
            bar->setProperty("innerScroll",true); bar->setProperty("scrollActive",false); bar->installEventFilter(this);
            bar->style()->unpolish(bar); bar->style()->polish(bar);
            connect(idle,&QTimer::timeout,bar,[bar,idle] {
                if (bar->isSliderDown()) { idle->start(); return; }
                bar->setProperty("scrollActive",false); bar->style()->unpolish(bar); bar->style()->polish(bar); bar->update();
            });
        }
    }
    for (auto *combo : root->findChildren<QComboBox *>()) { combo->setFocusPolicy(Qt::StrongFocus); combo->installEventFilter(this); }
    for (auto *spin : root->findChildren<QAbstractSpinBox *>()) { spin->setFocusPolicy(Qt::StrongFocus); spin->installEventFilter(this); }
}

bool TrainingPanel::eventFilter(QObject *watched,QEvent *event) {
    auto *widget=qobject_cast<QWidget *>(watched); if (!widget) return BasePanel::eventFilter(watched,event);
    QAbstractScrollArea *area=nullptr;
    for (auto *parent=widget;parent;parent=parent->parentWidget()) {
        if ((area=qobject_cast<QAbstractScrollArea *>(parent))) break;
    }
    const int scrollKey=event->type()==QEvent::KeyPress?static_cast<QKeyEvent *>(event)->key():0;
    const bool keyboardScroll=scrollKey==Qt::Key_Up || scrollKey==Qt::Key_Down || scrollKey==Qt::Key_Left ||
        scrollKey==Qt::Key_Right || scrollKey==Qt::Key_PageUp || scrollKey==Qt::Key_PageDown || scrollKey==Qt::Key_Home || scrollKey==Qt::Key_End;
    const bool scrolling=event->type()==QEvent::Wheel ||
        (keyboardScroll && area && (widget==area || widget==area->viewport() || qobject_cast<QScrollBar *>(widget))) ||
        (qobject_cast<QScrollBar *>(widget) && event->type()==QEvent::MouseButtonPress);
    if (scrolling && area && area->property("transientScroll").toBool()) {
        for (auto *bar : {area->verticalScrollBar(),area->horizontalScrollBar()}) {
            if (!bar->property("scrollActive").toBool()) {
                bar->setProperty("scrollActive",true); bar->style()->unpolish(bar); bar->style()->polish(bar); bar->update();
            }
        }
        area->findChild<QTimer *>("scrollIdleTimer")->start();
    }
    if (event->type()!=QEvent::Wheel) return BasePanel::eventFilter(watched,event);
    auto *wheel=static_cast<QWheelEvent *>(event);
    const auto delta=wheel->pixelDelta().isNull()?wheel->angleDelta():wheel->pixelDelta();
    if (delta.isNull()) return BasePanel::eventFilter(watched,event);
    auto *combo=qobject_cast<QComboBox *>(widget); auto *spin=qobject_cast<QAbstractSpinBox *>(widget);
    if (combo || spin) {
        if (wheel->modifiers()!=Qt::NoModifier || widget->hasFocus() || (combo && combo->view()->isVisible()) || !area) return BasePanel::eventFilter(watched,event);
        QWheelEvent forwarded(area->viewport()->mapFromGlobal(wheel->globalPosition().toPoint()),wheel->globalPosition(),
            wheel->pixelDelta(),wheel->angleDelta(),wheel->buttons(),wheel->modifiers(),wheel->phase(),wheel->inverted(),wheel->source());
        QApplication::sendEvent(area->viewport(),&forwarded); wheel->accept(); return true;
    }
    if (area && area->property("transientScroll").toBool() && !(wheel->modifiers() & Qt::ControlModifier)) {
        const bool horizontal=std::abs(delta.x())>std::abs(delta.y()) || (wheel->modifiers() & Qt::ShiftModifier);
        const auto *bar=horizontal?area->horizontalScrollBar():area->verticalScrollBar();
        const int direction=horizontal && delta.x()!=0?delta.x():delta.y();
        // A box owns its scroll gesture even at its limits; the page stays still.
        if ((direction<0 && bar->value()>=bar->maximum()) || (direction>0 && bar->value()<=bar->minimum())) {
            wheel->accept(); return true;
        }
    }
    return BasePanel::eventFilter(watched,event);
}

void TrainingPanel::addRunEvent(const QString &message,const QString &level,const QString &jobId,const QString &key,const QString &timestamp) {
    const auto id=jobId.isEmpty()?m_jobId:jobId; if (id.isEmpty() || message.trimmed().isEmpty()) return;
    const auto time=timestamp.isNull()?QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs):timestamp;
    const auto identity=key.isEmpty()?time+":"+level+":"+message:key;
    auto &keys=m_jobEventKeys[id]; if (keys.contains(identity)) return;
    keys.insert(identity); auto &entries=m_jobEvents[id];
    entries.append(QJsonObject{{"key",identity},{"time",time},{"level",level},{"message",message}});
    while (entries.size()>2000) { keys.remove(entries.first().toObject()["key"].toString()); entries.removeAt(0); }
    if (!m_batchEvents && id==m_displayedJob["id"].toString()) renderEventLog();
}

void TrainingPanel::recordJobEvents(const QJsonObject &job) {
    const auto id=job["id"].toString(); if (id.isEmpty()) { renderEventLog(); return; }
    m_batchEvents=true;
    const auto started=QDateTime::fromString(job["started_at"].toString(),Qt::ISODate);
    if (started.isValid()) addRunEvent("Training started · "+job["base_model"].toString(),"info",id,"started",job["started_at"].toString());
    for (const auto &value : job["history"].toArray()) {
        const auto row=value.toObject(); const auto step=row["step"];
        if (!step.isDouble() || !std::isfinite(step.toDouble()) || step.toDouble()<0) continue;
        QStringList values,fields;
        for (const auto &metric : {qMakePair(QString("loss"),QString("Loss")),qMakePair(QString("eval_loss"),QString("Validation")),
                qMakePair(QString("learning_rate"),QString("LR")),qMakePair(QString("grad_norm"),QString("Gradient"))}) {
            if (!row[metric.first].isDouble() || !std::isfinite(row[metric.first].toDouble())) continue;
            fields << metric.first; values << metric.second+" "+QString::number(row[metric.first].toDouble(),'g',6);
        }
        if (values.isEmpty()) continue;
        QString timestamp=QStringLiteral(""); const double elapsed=row["elapsed_seconds"].toDouble(-1);
        if (started.isValid() && std::isfinite(elapsed) && elapsed>=0 && elapsed<1e10) timestamp=started.addMSecs(qRound64(elapsed*1000)).toString(Qt::ISODateWithMs);
        const auto number=QString::number(step.toDouble(),'g',15);
        addRunEvent("Step "+number+" · "+values.join(" · "),row["eval_loss"].isDouble()?"success":"info",id,"sample:"+number+":"+fields.join(','),timestamp);
    }
    const auto state=job["status"].toString();
    if (state=="done" || state=="error" || state=="cancelled") {
        const auto message=job["message"].toString();
        addRunEvent(message.isEmpty()?(state=="done"?"Training completed":state=="cancelled"?"Training stopped":"Training failed"):message,
            state=="done"?"success":state=="error"?"error":"warn",id,"state:"+state,job["finished_at"].toString(QStringLiteral("")));
    }
    m_batchEvents=false; renderEventLog();
}

void TrainingPanel::renderEventLog() {
    if (!m_log) return;
    const auto id=m_displayedJob["id"].toString(); const auto entries=m_jobEvents.value(id);
    m_log->setAccessibleDescription("Events for "+m_displayedJob["name"].toString());
    const auto filter=m_logSearch->text().trimmed(); QStringList lines;
    for (const auto &value : entries) {
        const auto row=value.toObject(); const auto message=row["message"].toString(),level=row["level"].toString();
        if (!filter.isEmpty() && !message.contains(filter,Qt::CaseInsensitive) && !level.contains(filter,Qt::CaseInsensitive)) continue;
        const auto time=QDateTime::fromString(row["time"].toString(),Qt::ISODateWithMs);
        lines << (time.isValid()?time.toLocalTime().toString("HH:mm:ss"):"--:--:--")+"  "+("["+level.toUpper()+"]").leftJustified(9)+" "+message;
    }
    m_logCount->setText(filter.isEmpty()?QString("%1 events").arg(entries.size()):QString("%1 / %2 events").arg(lines.size()).arg(entries.size()));
    m_logCount->setToolTip("Recorded measurements and events for the selected experiment.");
    m_logCopy->setEnabled(!lines.isEmpty());
    m_log->setPlaceholderText(!filter.isEmpty()?"No events match this filter.":"Events for this experiment will appear here.");
    const auto text=lines.join('\n'); const bool sameJob=m_renderedLogJob==id;
    if (sameJob && text==m_renderedLogText) return;
    const auto cursor=m_log->textCursor(); const int anchor=cursor.anchor(),position=cursor.position();
    const int vertical=m_log->verticalScrollBar()->value(),horizontal=m_log->horizontalScrollBar()->value();
    m_renderingLog=true; m_log->setPlainText(text);
    if (sameJob) {
        auto restored=m_log->textCursor(); const int end=qMax(0,m_log->document()->characterCount()-1);
        restored.setPosition(qMin(anchor,end)); restored.setPosition(qMin(position,end),QTextCursor::KeepAnchor); m_log->setTextCursor(restored);
        m_log->horizontalScrollBar()->setValue(horizontal);
    }
    m_log->verticalScrollBar()->setValue(m_logFollow->isChecked()?m_log->verticalScrollBar()->maximum():sameJob?vertical:0);
    m_renderingLog=false; m_previousLogScroll=m_log->verticalScrollBar()->value();
    m_renderedLogJob=id; m_renderedLogText=text;
}
