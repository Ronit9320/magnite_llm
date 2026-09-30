#include <QTabWidget>
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
#include <QStandardItemModel>
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
    auto *tabs = new QTabWidget(this);
    layout->addWidget(tabs,1);
    auto makeCategory = [this,tabs](const QString &name) {
        auto *page = new QWidget(this);
        auto *column = new QVBoxLayout(page);
        column->setContentsMargins(20,20,20,20);
        column->setSpacing(16);
        tabs->addTab(page,name);
        return column;
    };
    auto *runtime = makeCategory("Runtime");
    auto *storage = makeCategory("Storage && privacy");
    auto *visual = makeCategory("Appearance");

    auto *engineBox = new QGroupBox("Engine", this);
    auto *engineForm = new QFormLayout(engineBox);
    m_engineOverride = new QComboBox(this);
    for (const auto &e : kEngines) m_engineOverride->addItem(e);
    auto *engineItems = qobject_cast<QStandardItemModel *>(m_engineOverride->model());
    engineItems->item(3)->setEnabled(false);
    engineItems->item(3)->setText("Hosted API (not implemented)");
    m_engineOverride->setToolTip(
        "Override automatic engine selection. 'Auto' picks vLLM if a GPU is present, "
        "else a compatible local engine. Applies to the next model load.");
    engineForm->addRow("Inference engine:", m_engineOverride);
    runtime->addWidget(engineBox);

    auto *dataBox = new QGroupBox("Storage", this);
    auto *dataForm = new QFormLayout(dataBox);
    m_dataDir = new QLineEdit(this);
    m_dataDir->setToolTip("Applied on the next app start. MAGNITE_HOME takes precedence. Existing data is not moved.");
    auto *dirBtn = new QPushButton("Browse...", this);
    dirBtn->setObjectName("secondaryBtn");
    auto *dirRow = new QHBoxLayout();
    dirRow->addWidget(m_dataDir, 1);
    dirRow->addWidget(dirBtn);
    dataForm->addRow("Data directory:", dirRow);
    connect(dirBtn, &QPushButton::clicked, this, &SettingsPanel::onBrowseDataDir);
    storage->addWidget(dataBox);

    auto *hostedBox = new QGroupBox("Hosted Inference", this);
    auto *hostedForm = new QFormLayout(hostedBox);
    m_hostedEndpoint = new QLineEdit(this);
    m_hostedEndpoint->setPlaceholderText("https://api.openai.com/v1");
    hostedForm->addRow("Endpoint (OpenAI-compatible):", m_hostedEndpoint);
    m_hostedKey = new QLineEdit(this);
    m_hostedKey->setEchoMode(QLineEdit::Password);
    m_hostedKey->setPlaceholderText("sk-...");
    m_hostedEndpoint->setEnabled(false);
    m_hostedKey->setEnabled(false);
    hostedBox->setTitle("Hosted Inference — not implemented");
    hostedForm->addRow("API key:", m_hostedKey);
    runtime->addWidget(hostedBox);
    runtime->addStretch();

    auto *privacyBox = new QGroupBox("Privacy", this);
    auto *privacyForm = new QFormLayout(privacyBox);
    m_telemetryOptIn = new QCheckBox("Send anonymous usage data", this);
    m_telemetryOptIn->setToolTip("Local-first by default. Opt-in only.");
    privacyForm->addRow("Telemetry:", m_telemetryOptIn);
    auto *appearance = Surface::card(this,"Appearance");
    auto *theme = new QLabel("Studio · paper, charcoal & orange",this);
    theme->setObjectName("bodyLabel");
    appearance->addWidget(theme);
    m_stars = new QCheckBox("Instrument grid",this);
    m_stars->setObjectName("starsToggle");
    m_reducedMotion = new QCheckBox("Reduce motion",this);
    m_reducedMotion->setObjectName("reducedMotionToggle");
    appearance->addWidget(m_stars);
    appearance->addWidget(m_reducedMotion);
    auto *hint = new QLabel("Subtle workspace texture. Reduce motion keeps activity indicators still and disables click effects, blinking cursors and panel transitions.",this);
    hint->setWordWrap(true);
    hint->setObjectName("cardMeta");
    appearance->addWidget(hint);
    visual->addWidget(appearance->parentWidget());
    visual->addStretch();
    storage->addWidget(privacyBox);
    auto *local = Surface::card(this,"Local by default");
    auto *copy = new QLabel("Models, adapters and documents stay in your local data directory.\n\nUse the Light / Dark switch in the top bar to change appearance.",this);
    copy->setWordWrap(true);
    local->addWidget(copy);
    storage->addWidget(local->parentWidget());
    storage->addStretch();

    auto *btnRow = new QHBoxLayout();
    m_saveButton = new QPushButton("Save Settings", this);
    m_saveButton->setObjectName("primaryBtn");
    btnRow->addWidget(m_saveButton);
    btnRow->addStretch();
    layout->addLayout(btnRow);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName("statusLabel");
    layout->addWidget(m_statusLabel);



    connect(m_saveButton, &QPushButton::clicked, this, &SettingsPanel::onSave);

    loadFromSettings();
}

void SettingsPanel::loadFromSettings() {
    QSettings s;
    s.remove("hosted/apiKey");
    m_engineOverride->setCurrentIndex(qBound(0, s.value("engine/override", 0).toInt(), 2));
    m_dataDir->setText(s.value("storage/dataDir").toString());
    m_hostedEndpoint->setText(s.value("hosted/endpoint").toString());
    m_hostedKey->clear();
    m_telemetryOptIn->setChecked(s.value("privacy/telemetry", false).toBool());
    m_stars->setChecked(s.value("appearance/studio3Ambient",true).toBool());
    m_reducedMotion->setChecked(s.value("appearance/studio3ReducedMotion",false).toBool());
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
    s.remove("hosted/apiKey"); // Do not retain credentials for an unavailable feature.
    s.setValue("privacy/telemetry", m_telemetryOptIn->isChecked());
    s.setValue("appearance/studio3Ambient",m_stars->isChecked());
    s.setValue("appearance/studio3ReducedMotion",m_reducedMotion->isChecked());
    s.sync();
    setStatus("Settings saved. Storage changes apply after restarting the app; existing data is not moved.");
    emit settingsChanged();
}

void SettingsPanel::setStatus(const QString &text) {
    m_statusLabel->setText(text);
}
