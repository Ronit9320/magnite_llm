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
#include <QFileInfo>
#include <QVBoxLayout>

namespace {
const QString kEngines[] = {"Auto (detect hardware)", "Local (vLLM)",
                            "Local (llama.cpp)"};
}

SettingsPanel::SettingsPanel(QWidget *parent)
    : BasePanel("Settings", parent)
    , m_engineOverride(nullptr)
    , m_dataDir(nullptr)
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
    m_engineOverride->setAccessibleName("Default inference engine");
    m_engineOverride->setToolTip(
        "Override automatic engine selection. 'Auto' picks vLLM if a GPU is present, "
        "else a compatible local engine. Applies to the next model load.");
    engineForm->addRow("Inference engine:", m_engineOverride);
    runtime->addWidget(engineBox);

    auto *dataBox = new QGroupBox("Storage", this);
    auto *dataForm = new QFormLayout(dataBox);
    m_dataDir = new QLineEdit(this);
    m_dataDir->setAccessibleName("Data directory");
    m_dataDir->setPlaceholderText(qEnvironmentVariable("MAGNITE_HOME", QDir::homePath()+"/.magnite"));
    m_dataDir->setToolTip("Applied on the next app start. MAGNITE_HOME takes precedence. Existing data is not moved.");
    auto *dirBtn = new QPushButton("Browse...", this);
    dirBtn->setObjectName("secondaryBtn");
    auto *dirRow = new QHBoxLayout();
    dirRow->addWidget(m_dataDir, 1);
    dirRow->addWidget(dirBtn);
    dataForm->addRow("Data directory:", dirRow);
    connect(dirBtn, &QPushButton::clicked, this, &SettingsPanel::onBrowseDataDir);
    storage->addWidget(dataBox);

    auto *engineHint = new QLabel("Auto chooses a compatible engine for your model and hardware. You can select torch or Ollama for an individual model in Playground → Model settings.",this);
    engineHint->setObjectName("sectionHint");
    engineHint->setWordWrap(true);
    runtime->addWidget(engineHint);
    runtime->addStretch();

    auto *appearance = Surface::card(this,"Appearance");
    auto *theme = new QLabel("Magnite · paper, graphite & orange",this);
    theme->setObjectName("bodyLabel");
    appearance->addWidget(theme);
    m_stars = new QCheckBox("Instrument grid",this);
    m_stars->setObjectName("starsToggle");
    m_reducedMotion = new QCheckBox("Reduce motion",this);
    m_reducedMotion->setObjectName("reducedMotionToggle");
    appearance->addWidget(m_stars);
    appearance->addWidget(m_reducedMotion);
    auto *hint = new QLabel("Changes apply immediately. Reduce motion keeps activity indicators still and disables click effects, blinking cursors and panel transitions.",this);
    hint->setWordWrap(true);
    hint->setObjectName("cardMeta");
    appearance->addWidget(hint);
    visual->addWidget(appearance->parentWidget());
    visual->addStretch();

    auto *local = Surface::card(this,"Local by default");
    auto *copy = new QLabel("Models, adapters and documents stay in your local data directory.\n\nUse the Light / Dark switch in the top bar to change appearance.",this);
    copy->setWordWrap(true);
    local->addWidget(copy);
    storage->addWidget(local->parentWidget());
    storage->addStretch();

    auto *btnRow = new QHBoxLayout();
    m_saveButton = new QPushButton("Save changes", this);
    m_saveButton->setObjectName("primaryBtn");
    btnRow->addWidget(m_saveButton);
    btnRow->addStretch();
    layout->addLayout(btnRow);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName("statusLabel");
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setTextFormat(Qt::PlainText);
    layout->addWidget(m_statusLabel);



    connect(m_saveButton, &QPushButton::clicked, this, &SettingsPanel::onSave);

    loadFromSettings();
    m_saveButton->setEnabled(false);
    auto markDirty = [this]() { m_saveButton->setEnabled(true); setStatus("You have unsaved changes."); };
    connect(m_dataDir,&QLineEdit::textChanged,this,markDirty);
    connect(m_engineOverride,qOverload<int>(&QComboBox::currentIndexChanged),this,markDirty);
    auto appearanceChanged = [this]() {
        QSettings s;
        s.setValue("appearance/studio3Ambient",m_stars->isChecked());
        s.setValue("appearance/studio3ReducedMotion",m_reducedMotion->isChecked());
        emit settingsChanged();
    };
    connect(m_stars,&QCheckBox::toggled,this,appearanceChanged);
    connect(m_reducedMotion,&QCheckBox::toggled,this,appearanceChanged);
}

void SettingsPanel::loadFromSettings() {
    QSettings s;
    s.remove("hosted/apiKey");
    m_engineOverride->setCurrentIndex(qBound(0, s.value("engine/override", 0).toInt(), 2));
    m_dataDir->setText(s.value("storage/dataDir").toString());
    s.remove("hosted/endpoint");
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
    const QString path = m_dataDir->text().trimmed();
    if (!path.isEmpty() && (!QDir::isAbsolutePath(path) || (QFileInfo::exists(path) && !QFileInfo(path).isDir()))) {
        setStatus("Choose an absolute directory path, or leave it empty to use the default.");
        applyStatusTone(m_statusLabel,"error");
        m_dataDir->setFocus();
        return;
    }
    QSettings s;
    s.setValue("engine/override", m_engineOverride->currentIndex());
    s.setValue("storage/dataDir", m_dataDir->text().trimmed());
    s.remove("hosted/apiKey"); // Do not retain credentials for an unavailable feature.
    s.setValue("appearance/studio3Ambient",m_stars->isChecked());
    s.setValue("appearance/studio3ReducedMotion",m_reducedMotion->isChecked());
    s.sync();
    setStatus("Settings saved. Storage changes apply after restarting the app; existing data is not moved.");
    applyStatusTone(m_statusLabel,"ok");
    m_saveButton->setEnabled(false);
    emit settingsChanged();
}

void SettingsPanel::setStatus(const QString &text) {
    m_statusLabel->setText(text);
    applyStatusTone(m_statusLabel,QString());
}
