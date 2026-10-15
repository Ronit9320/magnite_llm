#pragma once

#include "panels/basepanel.h"

class StudioCanvas;
class QComboBox;
class QGroupBox;
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

public slots:
    // Re-syncs the base-model and dataset lists from the backend.
    void refreshOptions();

private slots:
    void onStart();
    void onStop();
    void onQuantizationToggled();
    void onBrowseBaseModel();
    void onBrowseDataset();
    void onPollStatus();

private:
    void setupUi();
    void setupHyperparameters(QWidget *parent);
    void refreshModels();
    void refreshDatasets(const QString &selectId = QString());
    QString quantizationCode() const;
    void finishJob(const QString &message, const QString &color);
    // tone: "" neutral | "ok" | "error" | "warn" — drives #statusLabel state.
    void setStatus(const QString &text, const QString &tone = QString());

    BackendClient *m_client;
    QTimer *m_pollTimer;
    QString m_jobId;
    QString m_lastMessage;
    int m_lastLoggedStep;

    QComboBox *m_modelCombo;
    QComboBox *m_datasetCombo;
    QLineEdit *m_adapterNameEdit;
    QGroupBox *m_hyperBox;
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
    QPlainTextEdit *m_log;
    QLabel *m_statusLabel;
    QLabel *m_progressLabel = nullptr;
    QLabel *m_metrics = nullptr;
    QProgressBar *m_progress = nullptr;
    StudioCanvas *m_activity = nullptr;
    QWidget *m_setup = nullptr;
    QLabel *m_runHint = nullptr;
};