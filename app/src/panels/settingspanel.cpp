#include "panels/settingspanel.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include "widgets/surface.h"
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
    auto *columns = new QHBoxLayout();
    columns->setSpacing(24);
    auto *left = new QVBoxLayout();
    auto *right = new QVBoxLayout();
    left->setSpacing(24); right->setSpacing(24);
    columns->addLayout(left,1); columns->addLayout(right,1);
    layout->addLayout(columns);

    auto *engineBox = new QGroupBox("Engine", this);
    auto *engineForm = new QFormLayout(engineBox);
    m_engineOverride = new QComboBox(this);
    for (const auto &e : kEngines) m_engineOverride->addItem(e);
    m_engineOverride->setToolTip(
        "Override automatic engine selection. 'Auto' picks vLLM if a GPU is present, "
        "else llama.cpp (or a hosted API if configured).");
    engineForm->addRow("Inference engine:", m_engineOverride);
    left->addWidget(engineBox);

    auto *dataBox = new QGroupBox("Storage", this);
    auto *dataForm = new QFormLayout(dataBox);
    m_dataDir = new QLineEdit(this);
    auto *dirBtn = new QPushButton("Browse...", this);
    dirBtn->setObjectName("secondaryBtn");
    auto *dirRow = new QHBoxLayout();
    dirRow->addWidget(m_dataDir, 1);
    dirRow->addWidget(dirBtn);
    dataForm->addRow("Data directory:", dirRow);
    connect(dirBtn, &QPushButton::clicked, this, &SettingsPanel::onBrowseDataDir);
    left->addWidget(dataBox);

    auto *hostedBox = new QGroupBox("Hosted Inference", this);
    auto *hostedForm = new QFormLayout(hostedBox);
    m_hostedEndpoint = new QLineEdit(this);
    m_hostedEndpoint->setPlaceholderText("https://api.openai.com/v1");
    hostedForm->addRow("Endpoint (OpenAI-compatible):", m_hostedEndpoint);
    m_hostedKey = new QLineEdit(this);
    m_hostedKey->setEchoMode(QLineEdit::Password);
    m_hostedKey->setPlaceholderText("sk-...");
    hostedForm->addRow("API key:", m_hostedKey);
    left->addWidget(hostedBox);

    auto *privacyBox = new QGroupBox("Privacy", this);
    auto *privacyForm = new QFormLayout(privacyBox);
    m_telemetryOptIn = new QCheckBox("Send anonymous usage data", this);
    m_telemetryOptIn->setToolTip("Local-first by default. Opt-in only.");
    privacyForm->addRow("Telemetry:", m_telemetryOptIn);
    auto *appearance = Surface::card(this,"Appearance");
    auto *theme = new QLabel("Studio · graphite, mint & cream",this);
    theme->setObjectName("bodyLabel");
    appearance->addWidget(theme);
    m_stars = new QCheckBox("Ambient canvas",this);
    m_stars->setObjectName("starsToggle");
    m_reducedMotion = new QCheckBox("Reduce motion",this);
    m_reducedMotion->setObjectName("reducedMotionToggle");
    appearance->addWidget(m_stars);
    appearance->addWidget(m_reducedMotion);
    auto *hint = new QLabel("A quiet moving glow behind your workspace. Reduced motion freezes the glow and disables panel fades.",this);
    hint->setWordWrap(true);
    hint->setObjectName("cardMeta");
    appearance->addWidget(hint);
    right->addWidget(appearance->parentWidget());
    right->addWidget(privacyBox);
    auto *local = Surface::card(this,"Local by default");
    auto *copy = new QLabel("Models, adapters and documents stay in your local data directory.\n\nAdditional color themes will be explored later.",this);
    copy->setWordWrap(true);
    local->addWidget(copy);
    right->addWidget(local->parentWidget());
    right->addStretch();

    auto *btnRow = new QHBoxLayout();
    m_saveButton = new QPushButton("Save Settings", this);
    m_saveButton->setObjectName("primaryBtn");
    btnRow->addWidget(m_saveButton);
    btnRow->addStretch();
    layout->addLayout(btnRow);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName("statusLabel");
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
    m_stars->setChecked(s.value("appearance/studioAmbient",true).toBool());
    m_reducedMotion->setChecked(s.value("appearance/studioReducedMotion",false).toBool());
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
    s.setValue("appearance/studioAmbient",m_stars->isChecked());
    s.setValue("appearance/studioReducedMotion",m_reducedMotion->isChecked());
    s.sync();
    setStatus("Settings saved.");
    emit settingsChanged();
}

void SettingsPanel::setStatus(const QString &text) {
    m_statusLabel->setText(text);
}
