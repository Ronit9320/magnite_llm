#include "panels/settingspanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>

namespace {
const QString kEngines[] = {"Auto (detect hardware)", "Local (vLLM)",
                            "Local (llama.cpp)", "Hosted API"};
}

SettingsPanel::SettingsPanel(QWidget *parent)
    : BasePanel("Settings", parent)
    , m_engineOverride(nullptr)
    , m_dataDir(nullptr)
    , m_hostedEndpoint(nullptr)
    , m_hostedKey(nullptr)
    , m_telemetryOptIn(nullptr)
    , m_saveButton(nullptr)
    , m_statusLabel(nullptr)
{
    setHeaderSubtitle("Application and inference configuration.");
    setupUi();
}

void SettingsPanel::setupUi() {
    auto *layout = contentLayout();

    auto *engineBox = new QGroupBox("Engine", this);
    auto *engineForm = new QFormLayout(engineBox);
    m_engineOverride = new QComboBox(this);
    for (const auto &e : kEngines) m_engineOverride->addItem(e);
    m_engineOverride->setToolTip(
        "Override automatic engine selection. 'Auto' picks vLLM if a GPU is present, "
        "else llama.cpp (or a hosted API if configured).");
    engineForm->addRow("Inference engine:", m_engineOverride);
    layout->addWidget(engineBox);

    auto *dataBox = new QGroupBox("Storage", this);
    auto *dataForm = new QFormLayout(dataBox);
    m_dataDir = new QLineEdit(this);
    auto *dirBtn = new QPushButton("Browse...", this);
    auto *dirRow = new QHBoxLayout();
    dirRow->addWidget(m_dataDir, 1);
    dirRow->addWidget(dirBtn);
    dataForm->addRow("Data directory:", dirRow);
    connect(dirBtn, &QPushButton::clicked, this, &SettingsPanel::onBrowseDataDir);
    layout->addWidget(dataBox);

    auto *hostedBox = new QGroupBox("Hosted Inference", this);
    auto *hostedForm = new QFormLayout(hostedBox);
    m_hostedEndpoint = new QLineEdit(this);
    m_hostedEndpoint->setPlaceholderText("https://api.openai.com/v1");
    hostedForm->addRow("Endpoint (OpenAI-compatible):", m_hostedEndpoint);
    m_hostedKey = new QLineEdit(this);
    m_hostedKey->setEchoMode(QLineEdit::Password);
    m_hostedKey->setPlaceholderText("sk-...");
    hostedForm->addRow("API key:", m_hostedKey);
    layout->addWidget(hostedBox);

    auto *privacyBox = new QGroupBox("Privacy", this);
    auto *privacyForm = new QFormLayout(privacyBox);
    m_telemetryOptIn = new QCheckBox("Send anonymous usage data", this);
    m_telemetryOptIn->setToolTip("Local-first by default. Opt-in only.");
    privacyForm->addRow("Telemetry:", m_telemetryOptIn);
    layout->addWidget(privacyBox);

    auto *btnRow = new QHBoxLayout();
    m_saveButton = new QPushButton("Save Settings", this);
    btnRow->addWidget(m_saveButton);
    btnRow->addStretch();
    layout->addLayout(btnRow);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet("color: #666;");
    layout->addWidget(m_statusLabel);

    layout->addStretch();

    connect(m_saveButton, &QPushButton::clicked, this, &SettingsPanel::onSave);

    loadFromSettings();
}

void SettingsPanel::loadFromSettings() {
    QSettings s;
    m_engineOverride->setCurrentIndex(s.value("engine/override", 0).toInt());
    m_dataDir->setText(s.value("storage/dataDir").toString());
    m_hostedEndpoint->setText(s.value("hosted/endpoint").toString());
    m_hostedKey->setText(s.value("hosted/apiKey").toString());
    m_telemetryOptIn->setChecked(s.value("privacy/telemetry", false).toBool());
}

void SettingsPanel::onBrowseDataDir() {
    QString dir = QFileDialog::getExistingDirectory(
        this, "Select data directory",
        m_dataDir->text().isEmpty() ? QDir::homePath() : m_dataDir->text());
    if (!dir.isEmpty()) {
        m_dataDir->setText(dir);
    }
}

void SettingsPanel::onSave() {
    QSettings s;
    s.setValue("engine/override", m_engineOverride->currentIndex());
    s.setValue("storage/dataDir", m_dataDir->text().trimmed());
    s.setValue("hosted/endpoint", m_hostedEndpoint->text().trimmed());
    s.setValue("hosted/apiKey", m_hostedKey->text().trimmed());
    s.setValue("privacy/telemetry", m_telemetryOptIn->isChecked());
    s.sync();
    setStatus("Settings saved.");
    emit settingsChanged();
}

void SettingsPanel::setStatus(const QString &text) {
    m_statusLabel->setText(text);
}