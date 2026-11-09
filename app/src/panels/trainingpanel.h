#pragma once

#include "panels/basepanel.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QSet>

class StudioCanvas;
class SegmentedTabBar;
class QStackedWidget;
class QBoxLayout;
class TrainingChart;
class QListWidget;
class QComboBox;
class QLineEdit;
class QSpinBox;
class QDoubleSpinBox;
class QCheckBox;
class QPushButton;
class QLabel;
class QPlainTextEdit;
class QTimer;
class QProgressBar;
class BackendClient;
class QTableWidget;
class QGridLayout;
class QVBoxLayout;
class QDialog;
class QFrame;

// Training panel: configure and monitor LoRA/QLoRA fine-tuning jobs.
// Talks to the backend training/datasets API and polls live job status.
class TrainingPanel : public BasePanel {
    Q_OBJECT

public:
    explicit TrainingPanel(QWidget *parent = nullptr);

    void setBackendClient(BackendClient *client);

    QString settingsJson() const override;
    void applySettingsJson(const QString &json) override;

signals:
    void trainingCompleted();
    void runDetailsRequested(const QString &jobId);

public slots:
    // Re-syncs the base-model and dataset lists from the backend.
    void refreshOptions();

protected:
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void onStart();
    void onStop();
    void onQuantizationToggled();
    void onBrowseBaseModel();
    void onBrowseDataset();
    void onPollStatus();

private:
    void setupUi();
    void updateResponsiveLayout();
    void setWorkspace(int index);
    void renderJob(const QJsonObject &job);
    void selectRun(const QString &id);
    void refreshExperimentViews();
    void refreshTelemetry();
    void openRunDialog();
    void closeRunDialog();
    void applyDialogRecipe();
    QJsonObject dialogRecipe() const;
    void populateDialogRecipe(const QJsonObject &recipe);
    bool validateDialog();
    void requestDialogEstimate();
    void queueDialogEstimate();
    void refreshRunAppearance();
    void wireScrolling(QWidget *root);
    void recordJobEvents(const QJsonObject &job);
    void addRunEvent(const QString &message,const QString &level=QStringLiteral("info"),
                     const QString &jobId=QString(),const QString &key=QString(),const QString &timestamp=QString());
    void renderEventLog();
    void reportPollingStatus(const QString &message, const QString &tone = QString());
    void updateRecipeSummary();
    void filterRuns();
    void reuseRun();
    void refreshJobs();
    void displayJob(const QJsonObject &job);
    void updateInputSummary();
    void requestEstimate();
    void setupHyperparameters(QWidget *parent);
    void updateComparison();
    void refreshComparisonOptions();
    void refreshModels();
    void refreshDatasets(const QString &selectId = QString());
    // Asks the backend whether 4-bit/8-bit QLoRA can run here (not on macOS).
    void refreshCapabilities();
    void applyQuantizationAvailability();
    QString quantizationCode() const;
    void finishJob(const QString &message, const QString &color);
    // tone: "" neutral | "ok" | "error" | "warn" — drives #statusLabel state.
    void setStatus(const QString &text, const QString &tone = QString(), bool dialogFeedback = true);

    QGridLayout *m_workspaceGrid = nullptr;
    QVBoxLayout *m_centerLayout = nullptr;
    QVBoxLayout *m_railLayout = nullptr;
    QWidget *m_center = nullptr;
    QWidget *m_rail = nullptr;
    QWidget *m_indexCard = nullptr;
    QWidget *m_computeCard = nullptr;
    QWidget *m_configCard = nullptr;
    QWidget *m_activityCard = nullptr;
    QWidget *m_recipeInputs = nullptr;
    QWidget *m_comparisonPanel = nullptr;
    QVBoxLayout *m_dialogLayout = nullptr;
    QDialog *m_runDialog = nullptr;
    QTableWidget *m_experiments = nullptr;
    QHash<QString,QLabel *> m_configValues;
    QHash<QString,QLabel *> m_computeValues;
    QList<QFrame *> m_utilSegments;
    QList<QLabel *> m_statNotes;
    QLabel *m_badge = nullptr;
    QLabel *m_stepLabel = nullptr;
    QLabel *m_epochLabel = nullptr;
    QLabel *m_legend = nullptr;
    QLabel *m_checkpointLabel = nullptr;
    QLabel *m_activityText = nullptr;
    QLabel *m_recipeSummary = nullptr;
    QLabel *m_recipeFacts = nullptr;
    QLabel *m_budgetSummary = nullptr;
    QLabel *m_pageTitle = nullptr;
    QLabel *m_pageSubtitle = nullptr;
    QLabel *m_breadcrumb = nullptr;
    QLabel *m_compareSummary = nullptr;
    QCheckBox *m_smoothing = nullptr;
    TrainingChart *m_throughputChart = nullptr;
    QString m_selectedRunId;
    QTimer *m_telemetryTimer = nullptr;
    bool m_telemetryPending = false;
    bool m_configuring = false;
    QHash<QString,QWidget *> m_dialogEditors;
    QLabel *m_dialogStatus = nullptr;
    QLabel *m_dialogEstimate = nullptr;
    QTimer *m_dialogEstimateTimer = nullptr;
    int m_dialogEstimateGeneration = 0;
    bool m_cancelPending = false;
    bool m_runHealthWarning = false;
    QString m_runHealthMessage;
    QFrame *m_feedbackFrame = nullptr;
    QLabel *m_runEyebrow = nullptr;
    QLabel *m_timeLabel = nullptr;
    QLabel *m_historyEmpty = nullptr;
    QLabel *m_indexEmpty = nullptr;
    QPushButton *m_followActive = nullptr;
    SegmentedTabBar *m_workspaceTabs = nullptr;
    SegmentedTabBar *m_metricTabs = nullptr;
    QStackedWidget *m_chartStack = nullptr;
    QWidget *m_observatorySignals = nullptr;
    TrainingChart *m_observatoryRate = nullptr;
    TrainingChart *m_observatoryGradient = nullptr;
    QLabel *m_validationReading = nullptr;
    QLabel *m_throughputReading = nullptr;
    QLabel *m_remainingReading = nullptr;
    QLabel *m_runConfiguration = nullptr;
    QLabel *m_runMeta = nullptr;
    QJsonObject m_displayedJob;
    BackendClient *m_client;
    QTimer *m_pollTimer;
    QString m_jobId;
    QString m_lastMessage;

    QComboBox *m_modelCombo;
    QComboBox *m_datasetCombo;
    QLineEdit *m_adapterNameEdit;
    QWidget *m_hyperBox;
    QComboBox *m_targetModules;
    QSpinBox *m_rankSpin;
    QSpinBox *m_alphaSpin;
    QComboBox *m_quantCombo;
    QSpinBox *m_epochsSpin;
    QDoubleSpinBox *m_lrSpin;
    QSpinBox *m_batchSpin;
    QDoubleSpinBox *m_warmupSpin;
    QSpinBox *m_maxLengthSpin;
    QCheckBox *m_qloraCheck;
    QPushButton *m_startButton;
    QPushButton *m_stopButton;
    QPushButton *m_loadPreset = nullptr;
    QPlainTextEdit *m_log;
    QWidget *m_logBody = nullptr;
    QPushButton *m_logToggle = nullptr;
    QPushButton *m_logFollow = nullptr;
    QPushButton *m_logCopy = nullptr;
    QLabel *m_logCount = nullptr;
    QLineEdit *m_logSearch = nullptr;
    QHash<QString,QJsonArray> m_jobEvents;
    QHash<QString,QSet<QString>> m_jobEventKeys;
    QString m_renderedLogJob;
    QString m_renderedLogText;
    bool m_batchEvents = false;
    bool m_renderingLog = false;
    bool m_appearanceRefreshPending = false;
    int m_previousLogScroll = 0;
    QLabel *m_statusLabel;
    QLabel *m_progressLabel = nullptr;
    QLabel *m_metrics = nullptr;
    QProgressBar *m_progress = nullptr;
    StudioCanvas *m_activity = nullptr;
    QWidget *m_setup = nullptr;
    QLabel *m_runHint = nullptr;
    TrainingChart *m_lossChart = nullptr;
    TrainingChart *m_lrChart = nullptr;
    TrainingChart *m_gradChart = nullptr;
    QListWidget *m_runs = nullptr;
    QLabel *m_inputSummary = nullptr;
    QLabel *m_datasetSummary = nullptr;
    QLabel *m_runName = nullptr;
    QDoubleSpinBox *m_dropoutSpin = nullptr;
    QDoubleSpinBox *m_validationSpin = nullptr;
    QTimer *m_estimateTimer = nullptr;
    QLabel *m_estimateLabel = nullptr;
    QProgressBar *m_memoryBar = nullptr;
    QCheckBox *m_visualise = nullptr;
    QWidget *m_chartTabs = nullptr;
    QPlainTextEdit *m_rawData = nullptr;
    QWidget *m_archive = nullptr;
    QPushButton *m_archiveToggle = nullptr;
    QPushButton *m_settingsToggle = nullptr;
    QLabel *m_lossReading = nullptr;
    QLabel *m_rateReading = nullptr;
    QLabel *m_gradientReading = nullptr;
    QComboBox *m_compareA = nullptr;
    QComboBox *m_compareB = nullptr;
    QTableWidget *m_comparisonTable = nullptr;
    QLabel *m_comparisonHint = nullptr;
    TrainingChart *m_compareLoss = nullptr;
    TrainingChart *m_compareRate = nullptr;
    TrainingChart *m_compareGradient = nullptr;
    int m_estimateRequest = 0;
    QJsonArray m_jobs;
    QJsonArray m_models;
    QJsonArray m_datasets;
    bool m_pollPending = false;
    int m_jobsRequest = 0;
    bool m_quantizationAvailable = true;
    QString m_quantizationDetail;
};
