#include "panels/settingspanel.h"
#include "backend_client.h"
#include "panels/chatpanel.h"
#include "panels/chatstore.h"
#include "panels/greeting.h"
#include "theme.h"
#include "update_config.h"
#include "build_features.h"
#include "update_manager.h"
#include "widgets/brandmark.h"
#include "widgets/button.h"
#include "widgets/combobox.h"
#include "widgets/confirmation.h"
#include "widgets/contextmeter.h"
#include "widgets/motion.h"
#include "widgets/settingssummary.h"
#include "widgets/settingswindow.h"
#include "widgets/shortcuts.h"
#include "widgets/themechoice.h"
#include "widgets/toggleswitch.h"
#include <QApplication>
#include <QButtonGroup>
#include <QClipboard>
#include <QCoreApplication>
#include <QDate>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDir>
#include <QEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPlainTextEdit>
#include <QPointer>
#include <QPushButton>
#include <QSaveFile>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTimer>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>
#include <memory>

namespace {
// Page indexes are stable (other code selects them by number); Profile is listed first in the menu.
const QStringList titles = {"General",           "Appearance",    "Chat",        "Runtime",
                            "Storage & privacy", "Notifications", "Diagnostics", "Updates", "Profile"};
const QStringList icons = {"settings", "layers", "message-circle", "cpu",
                           "database", "box",    "file-text",      "refresh-cw", "user"};
constexpr int kProfilePage = 8;
// A one-line label that keeps its own font's line height as an explicit minimum, so a
// layout that measured it before its stylesheet font applied can never squeeze it.
class LineLabel : public QLabel {
public:
    using QLabel::QLabel;

protected:
    void changeEvent(QEvent* event) override {
        QLabel::changeEvent(event);
        if (event->type() == QEvent::FontChange || event->type() == QEvent::StyleChange)
            setMinimumHeight(fontMetrics().height() + contentsMargins().top() + contentsMargins().bottom());
    }
};
QLabel* label(const QString& text, const char* name, QWidget* parent = nullptr) {
    auto* result = new QLabel(text, parent);
    result->setObjectName(name);
    result->setWordWrap(true);
    result->setTextFormat(Qt::PlainText);
    return result;
}
QPushButton* button(const QString& text, const char* name = "secondaryBtn") {
    auto* result = new Button(text);
    result->setObjectName(name);
    return result;
}
void line(QVBoxLayout* layout) {
    auto* rule = new QFrame;
    rule->setObjectName("settingsRule");
    rule->setFixedHeight(1);
    layout->addWidget(rule);
}
void row(QVBoxLayout* layout, const QString& title, const QString& description,
         QWidget* control = nullptr) {
    auto* host = new QWidget;
    host->setObjectName("settingsRow");
    auto* body = new QHBoxLayout(host);
    body->setContentsMargins(0, 17, 0, 17);
    body->setSpacing(20);
    auto* copy = new QVBoxLayout;
    copy->setSpacing(6);
    copy->addWidget(label(title, "settingsRowTitle"));
    copy->addWidget(label(description, "settingsHint"));
    body->addLayout(copy, 1);
    if (control)
        body->addWidget(control, 0, Qt::AlignVCenter);
    layout->addWidget(host);
    line(layout);
}
void note(QVBoxLayout* layout, const QString& title, const QString& text) {
    auto* host = new QFrame;
    host->setObjectName("settingsNote");
    auto* body = new QVBoxLayout(host);
    body->setContentsMargins(16, 14, 16, 14);
    body->setSpacing(8);
    body->addWidget(label(title, "settingsRowTitle"));
    body->addWidget(label(text, "settingsHint"));
    layout->addWidget(host);
}
QWidget* signature(bool large) {
    auto* host = new QWidget;
    host->setObjectName(large ? "settingsUpdateBrand" : "settingsSidebarBrand");
    auto* body = new QVBoxLayout(host);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(6);
    body->addWidget(new BrandLockup(large ? 232 : 150, host));
    body->addWidget(label(QCoreApplication::applicationVersion(), "settingsHint"));
    return host;
}
} // namespace

SettingsPanel::SettingsPanel(QWidget* parent) : BasePanel("Settings", parent, true) {
    setupUi();
}
void SettingsPanel::setupUi() {
    setObjectName("settingsPanel");
    auto* root = contentLayout();
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto* shell = new QHBoxLayout;
    shell->setContentsMargins(0, 0, 0, 0);
    shell->setSpacing(0);
    root->addLayout(shell);
    auto* nav = new QWidget;
    nav->setObjectName("settingsNavigation");
    nav->setFixedWidth(218);
    // The sidebar scrolls when it is taller than the window (large interface sizes,
    // short screens) instead of squeezing its labels.
    auto* navFrame = new QVBoxLayout(nav);
    navFrame->setContentsMargins(0, 0, 0, 0);
    auto* navScroll = new QScrollArea;
    navScroll->setObjectName("settingsScroll");
    navScroll->setFrameShape(QFrame::NoFrame);
    navScroll->setWidgetResizable(true);
    navScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    navFrame->addWidget(navScroll);
    auto* navContent = new QWidget;
    navScroll->setWidget(navContent);
    auto* navBody = new QVBoxLayout(navContent);
    navBody->setContentsMargins(16, 28, 16, 22);
    navBody->setSpacing(4);
    navBody->addWidget(label("Settings", "settingsNavTitle"));
    navBody->addSpacing(22);
    for (int i = 0; i < titles.size(); ++i) {
        if (i == 7) {
            navBody->addSpacing(10);
            line(navBody);
            navBody->addSpacing(10);
        }
        auto* item = button(QString(titles[i]).replace("&", "&&"), "settingsCategory");
        item->setCheckable(true);
        item->setFlat(true);
        item->setProperty("leftAligned", true);
        item->setProperty("category", i);
        item->setAccessibleName(titles[i]);
        item->setIconSize(QSize(17, 17));
        m_navigation.append(item);
        if (i == kProfilePage) {
            navBody->insertWidget(2, item);   // above General: your workspace comes first
            navBody->insertSpacing(3, 8);
        } else
            navBody->addWidget(item);
        connect(item, &QPushButton::clicked, this, [this, i] { selectCategory(i); });
    }
    navBody->addStretch();
    navBody->addWidget(signature(false));
    shell->addWidget(nav);
    auto* right = new QWidget;
    right->setObjectName("settingsContent");
    auto* rightBody = new QVBoxLayout(right);
    rightBody->setContentsMargins(34, 26, 24, 0);
    rightBody->setSpacing(0);
    auto* heading = new QHBoxLayout;
    auto* headingCopy = new QVBoxLayout;
    headingCopy->setSpacing(8);
    m_title = label("", "settingsTitle");
    m_title->setProperty("windowDragRegion", true);
    headingCopy->addWidget(m_title);
    heading->addLayout(headingCopy, 1);
    auto* close = button("×", "settingsClose");
    close->setAccessibleName("Close settings");
    close->setFixedSize(30, 30);
    heading->addWidget(close, 0, Qt::AlignTop);
    connect(close, &QPushButton::clicked, this, &SettingsPanel::closeRequested);
    rightBody->addLayout(heading);
    rightBody->addSpacing(24);
    m_pages = new QStackedWidget;
    m_pages->setObjectName("settingsPages");
    QList<QVBoxLayout*> columns;
    for (int i = 0; i < titles.size(); ++i) {
        auto* scroll = new QScrollArea;
        scroll->setObjectName("settingsScroll");
        scroll->setFrameShape(QFrame::NoFrame);
        scroll->setWidgetResizable(true);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        auto* page = new QWidget;
        auto* column = new QVBoxLayout(page);
        column->setContentsMargins(0, 0, 12, 20);
        column->setSpacing(10);
        scroll->setWidget(page);
        m_pages->addWidget(scroll);
        columns << column;
    }
    rightBody->addWidget(m_pages, 1);
    line(rightBody);
    m_statusLabel = label("Changes save automatically on this device.", "settingsFooter");
    m_statusLabel->setAccessibleName("Settings status");
    rightBody->addWidget(m_statusLabel);
    shell->addWidget(right, 1);
    auto* general = columns[0];
    general->addWidget(label("What should we call you?", "settingsRowTitle"));
    m_profileName = new QLineEdit;
    m_profileName->setObjectName("profileName");
    m_profileName->setAccessibleName("Your name");
    m_profileName->setPlaceholderText("Your name (optional)");
    m_profileName->setMaxLength(80);
    general->addWidget(m_profileName);
    general->addWidget(label("Used in the greeting when you start a new chat.", "settingsHint"));
    note(general, "Just for your workspace",
         "Your name stays on this computer. Leave it empty for a greeting without a name.");
    auto* shortcuts = button("View shortcuts");
    shortcuts->setObjectName("settingsShortcuts");
    row(general, "Keyboard shortcuts", "Review the shortcuts available throughout Magnite Space.",
        shortcuts);
    connect(shortcuts, &QPushButton::clicked, this, &SettingsPanel::showShortcuts);
    auto* reset = button("Reset…");
    reset->setObjectName("resetPreferencesButton");
    row(general, "Reset preferences",
        "Restore app preferences. Saved content and your data directory stay in place.", reset);
    connect(reset, &QPushButton::clicked, this, &SettingsPanel::resetPreferences);
    auto* appearance = columns[1];
    m_theme = new ComboBox(this);
    m_theme->setObjectName("settingsTheme");
    m_theme->addItem("Light", "light");
    m_theme->addItem("Dark", "dark");
    m_theme->addItem("System", "system");
#if QT_VERSION < QT_VERSION_CHECK(6, 5, 0)
    m_theme->setToolTip("System uses the desktop appearance at launch on this Qt version. Restart "
                        "after changing your system theme.");
#endif
    m_theme->hide();
    appearance->addWidget(label("Color theme", "settingsRowTitle"));
    auto* choices = new QHBoxLayout;
    choices->setSpacing(12);
    auto* themeGroup = new QButtonGroup(this);
    themeGroup->setExclusive(true);
    for (int i = 0; i < 3; ++i) {
        auto* choice = new ThemeChoice(m_theme->itemData(i).toString());
        choice->setObjectName("themeChoice" + QString::number(i));
        themeGroup->addButton(choice, i);
        choices->addWidget(choice);
        connect(choice, &QPushButton::clicked, this, [this, i] { m_theme->setCurrentIndex(i); });
    }
    appearance->addLayout(choices);
    appearance->addWidget(
        label("Choose a theme, or let Magnite Space follow your system.", "settingsHint"));
    connect(m_theme, qOverload<int>(&QComboBox::currentIndexChanged), this,
            [themeGroup](int index) {
                if (auto* choice = themeGroup->button(index))
                    choice->setChecked(true);
            });
    m_grid = new ToggleSwitch("", this);
    m_grid->setObjectName("gridToggle");
    m_grid->setAccessibleName("Instrument grid");
    row(appearance, "Instrument grid", "A subtle grid behind your workspace.", m_grid);
    m_reducedMotion = new ToggleSwitch("", this);
    m_reducedMotion->setObjectName("reducedMotionToggle");
    m_reducedMotion->setAccessibleName("Reduce motion");
    row(appearance, "Reduce motion", "Keep activity indicators still and simplify animations.",
        m_reducedMotion);
    m_startMaximized = new ToggleSwitch("", this);
    m_startMaximized->setObjectName("startMaximizedToggle");
    m_startMaximized->setAccessibleName("Open maximized");
    row(appearance, "Open maximized", "Fill the screen when Magnite Space starts.",
        m_startMaximized);
    m_scale = new ComboBox;
    m_scale->setObjectName("settingsInterfaceSize");
    m_scale->addItems({"Compact", "Default", "Large"});
    row(appearance, "Interface size", "Adjust text size across Magnite Space.", m_scale);
    auto* chat = columns[2];
    m_sendKey = new ComboBox;
    m_sendKey->setObjectName("sendKeyCombo");
    m_sendKey->addItem("Enter to send", "enter");
    m_sendKey->addItem(Shortcuts::label(Shortcuts::modifiedSend()), "modified");
    row(chat, "Send message with",
        Shortcuts::label(Shortcuts::newLine()) + " always adds a new line.", m_sendKey);
    m_meterStyle = new ComboBox;
    m_meterStyle->setObjectName("meterStyleCombo");
    m_meterStyle->addItem("Ring", "ring");
    m_meterStyle->addItem("Bar", "bar");
    m_meterStyle->addItem("Ring + banner", "ringBanner");
    m_meterStyle->addItem("Off", "off");
    row(chat, "Context meter", "Show context usage. Warnings begin at 70%.", m_meterStyle);
    m_autoCompact = new ComboBox;
    m_autoCompact->setObjectName("autoCompactCombo");
    m_autoCompact->addItem("Off", 0);
    for (int percent = 70; percent <= 90; percent += 5)
        m_autoCompact->addItem(QString::number(percent) + "%", percent);
    row(chat, "Auto-compact", "Summarize older messages when you send at this threshold.",
        m_autoCompact);
    note(chat, "More room for the conversation",
         "Compacting summarizes older context for the model. Your original messages stay visible "
         "in the chat.");
    auto* runtime = columns[3];
    m_engineOverride = new ComboBox;
    m_engineOverride->setAccessibleName("Default inference engine");
    m_engineOverride->addItems({"Auto (detect hardware)", "Local (vLLM)", "Local (llama.cpp)"});
    row(runtime, "Default inference engine",
        "Auto chooses a compatible engine for your model and hardware.", m_engineOverride);
    note(runtime, "Applies to the next model you load",
         "Your current model keeps its existing engine. Individual model overrides, including "
         "torch and Ollama, live in Conversation → Model settings.");
    m_saveButton = button("Save changes", "primaryBtn");
    runtime->addWidget(m_saveButton, 0, Qt::AlignLeft);
    connect(m_saveButton, &QPushButton::clicked, this, &SettingsPanel::onSave);
    auto* details = button("View diagnostics");
    row(runtime, "Local runtime",
        "Check backend connectivity, installed engines and detected hardware.", details);
    connect(details, &QPushButton::clicked, this, [this] { selectCategory(6); });
    auto* storage = columns[4];
    m_storageInfo = new StorageSummary;
    storage->addWidget(m_storageInfo);
    m_effectivePath = label("", "settingsHint");
    storage->addWidget(label("Active data directory", "settingsRowTitle"));
    storage->addWidget(m_effectivePath);
    m_dataDir = new QLineEdit;
    m_dataDir->setObjectName("settingsDataDirectory");
    m_dataDir->setAccessibleName("Data directory");
    m_dataDir->setPlaceholderText("Default data directory");
    auto* paths = new QHBoxLayout;
    paths->addWidget(m_dataDir, 1);
    auto* browse = button("Browse…");
    auto* open = button("Open");
    paths->addWidget(browse);
    paths->addWidget(open);
    storage->addLayout(paths);
    connect(browse, &QPushButton::clicked, this, &SettingsPanel::onBrowseDataDir);
    connect(open, &QPushButton::clicked, this, [this] {
        if (!m_activeDataPath.isEmpty() &&
            !QDesktopServices::openUrl(QUrl::fromLocalFile(m_activeDataPath)))
            setStatus("Could not open the data directory.");
    });
    storage->addWidget(label(
        "Changes apply after restart; existing data is not moved. MAGNITE_HOME takes precedence.",
        "settingsHint"));
    m_storageSave = button("Save changes", "primaryBtn");
    storage->addWidget(m_storageSave, 0, Qt::AlignLeft);
    connect(m_storageSave, &QPushButton::clicked, this, &SettingsPanel::onSave);
    m_cachePath =
        label("Connect to the local service to resolve the shared model cache.", "settingsHint");
    storage->addWidget(label("Hugging Face model cache", "settingsRowTitle"));
    storage->addWidget(m_cachePath);
    auto* cache = button("Open cache folder");
    storage->addWidget(cache, 0, Qt::AlignLeft);
    connect(cache, &QPushButton::clicked, this, [this] {
        if (m_cacheDirectory.isEmpty()) {
            setStatus("The cache path is not available yet.");
            return;
        }
        if (!QDesktopServices::openUrl(QUrl::fromLocalFile(m_cacheDirectory)))
            setStatus("Could not open the cache folder.");
    });
    auto* models = button("Manage models");
    row(storage, "Downloaded models", "Review and remove model files individually in the Library.",
        models);
    connect(models, &QPushButton::clicked, this, &SettingsPanel::modelsRequested);
    auto* partialHeader = new QHBoxLayout;
    partialHeader->setContentsMargins(0, 17, 0, 0);
    partialHeader->setSpacing(10);
    auto* partialCopy = new QVBoxLayout;
    partialCopy->setSpacing(6);
    partialCopy->addWidget(label("Partial downloads", "settingsRowTitle"));
    m_partialSummary = label("Connect to the local service to check for unfinished downloads.",
                             "settingsHint");
    partialCopy->addWidget(m_partialSummary);
    partialHeader->addLayout(partialCopy, 1);
    m_partialScan = button("Scan again");
    m_partialScan->setObjectName("partialScan");
    m_partialDeleteAll = button("Delete all…", "destructiveBtn");
    m_partialDeleteAll->hide();
    partialHeader->addWidget(m_partialScan, 0, Qt::AlignVCenter);
    partialHeader->addWidget(m_partialDeleteAll, 0, Qt::AlignVCenter);
    storage->addLayout(partialHeader);
    m_partialHost = new QWidget;
    m_partialHost->setObjectName("partialDownloads");
    auto* partialBody = new QVBoxLayout(m_partialHost);
    partialBody->setContentsMargins(0, 0, 0, 0);
    partialBody->setSpacing(0);
    storage->addWidget(m_partialHost);
    line(storage);
    connect(m_partialScan, &QPushButton::clicked, this, &SettingsPanel::refreshPartials);
    connect(m_partialDeleteAll, &QPushButton::clicked, this, [this] {
        QStringList repos;
        for (const auto& partial : m_partials) repos << partial["repo"].toString();
        deletePartials(repos);
    });
    auto* exportButton = button("Export JSON");
    exportButton->setObjectName("exportAllChats");
    m_chatCount = label("", "settingsHint");
    storage->addWidget(m_chatCount);
    row(storage, "Export chats", "Save messages, branches and chat metadata to one JSON file.",
        exportButton);
    connect(exportButton, &QPushButton::clicked, this, &SettingsPanel::exportChats);
    m_deleteChats = button("Delete chats…", "destructiveBtn");
    m_deleteChats->setObjectName("deleteAllChats");
    row(storage, "Delete all chats",
        "Permanently remove saved conversations. Other workspace data stays in place.",
        m_deleteChats);
    connect(m_deleteChats, &QPushButton::clicked, this, &SettingsPanel::deleteChats);
    note(storage, "Local by default",
         "Hub search, downloads and update checks use the internet. Inference and training run "
         "locally. The shared model cache can also be used by other applications.");
    auto* profile = columns[kProfilePage];
    {
        auto* card = new QFrame;
        card->setObjectName("settingsStorageOverview");
        auto* body = new QHBoxLayout(card);
        body->setContentsMargins(20, 18, 20, 18);
        body->setSpacing(16);
        m_profileAvatar = new QLabel;
        m_profileAvatar->setObjectName("settingsProfileAvatar");
        m_profileAvatar->setFixedSize(52, 52);
        m_profileAvatar->setAlignment(Qt::AlignCenter);
        m_profileAvatar->setAccessibleName("Profile");
        body->addWidget(m_profileAvatar, 0, Qt::AlignTop);
        auto* copy = new QVBoxLayout;
        copy->setSpacing(4);
        m_profileTitle = label("Your workspace", "settingsStorageTotal");
        copy->addWidget(m_profileTitle);
        m_profileSince = label("Everything below is counted from this computer.", "settingsHint");
        copy->addWidget(m_profileSince);
        // Actions sit under the name so the card fits the narrow Settings window at every text size.
        auto* actions = new QHBoxLayout;
        actions->setContentsMargins(0, 8, 0, 0);
        actions->setSpacing(8);
        auto* editName = button("Edit name");
        connect(editName, &QPushButton::clicked, this, [this] {
            selectCategory(0);
            m_profileName->setFocus();
        });
        actions->addWidget(editName);
        auto* refresh = button("Refresh");
        refresh->setObjectName("profileRefresh");
        connect(refresh, &QPushButton::clicked, this, &SettingsPanel::refreshProfile);
        actions->addWidget(refresh);
        actions->addStretch();
        copy->addLayout(actions);
        body->addLayout(copy, 1);
        profile->addWidget(card);
    }
    profile->addSpacing(6);
    profile->addWidget(label("YOUR WORK", "settingsSectionEyebrow"));
    {
        struct Stat { const char *key, *title, *icon; };
        const Stat stats[] = {{"models", "Models", "box"},           {"adapters", "Adapters", "layers"},
                              {"training", "Training runs", "graduation-cap"}, {"datasets", "Datasets", "file-text"},
                              {"knowledge", "Knowledge documents", "database"}, {"blueprints", "Blueprints", "layers"},
                              {"tasks", "Tool tasks", "cpu"},       {"conversations", "Conversations", "message-circle"}};
        auto* grid = new QGridLayout;
        grid->setSpacing(10);
        int index = 0;
        for (const auto& stat : stats) {
            const QString key = stat.key;
            if (!Features::blueprintsAndTools && (key == "blueprints" || key == "tasks"))
                continue; // Not part of this build.
            auto* tile = new QFrame;
            tile->setObjectName("settingsMetric");
            tile->setProperty("profileStat", stat.key);
            auto* l = new QVBoxLayout(tile);
            l->setContentsMargins(16, 14, 16, 14);
            l->setSpacing(4);
            auto* head = new QHBoxLayout;
            head->setSpacing(7);
            auto* icon = new QLabel;
            icon->setProperty("themeIcon", stat.icon);
            icon->setPixmap(Theme::coloredIcon(stat.icon, QColor(Theme::textSecondary()), 14).pixmap(14, 14));
            head->addWidget(icon);
            head->addWidget(label(stat.title, "settingsHint"), 1);
            l->addLayout(head);
            // A short number on one line; see LineLabel.
            auto* value = new LineLabel("—");
            value->setObjectName("settingsProfileValue");
            value->setTextFormat(Qt::PlainText);
            value->setAccessibleName(stat.title);
            m_statValues.insert(stat.key, value);
            l->addWidget(value);
            auto* detail = label(" ", "settingsHint");
            m_statDetails.insert(stat.key, detail);
            l->addWidget(detail);
            grid->addWidget(tile, index / 2, index % 2);
            ++index;
        }
        grid->setColumnStretch(0, 1);
        grid->setColumnStretch(1, 1);
        profile->addLayout(grid);
    }
    m_profileStatus = label("Counts come from data stored on this computer. Nothing leaves your device.",
                            "settingsHint");
    profile->addWidget(m_profileStatus);
    auto* notifications = columns[5];
    note(notifications, "Keep working while Magnite Space runs",
         "Desktop notifications appear when Magnite Space is in the background and your system supports "
         "them. Your system controls notification permissions.");
    m_trainingNotice = new ToggleSwitch("", this);
    m_trainingNotice->setAccessibleName("Training notifications");
    row(notifications, "Training finished or failed",
        "Know when a run completes or needs attention.", m_trainingNotice);
    m_downloadNotice = new ToggleSwitch("", this);
    m_downloadNotice->setAccessibleName("Download notifications");
    row(notifications, "Download finished or failed",
        "Know when a model is ready or a download needs attention.", m_downloadNotice);
    m_sound = new ToggleSwitch("", this);
    m_sound->setAccessibleName("Notification sound");
    row(notifications, "Notification sound", "Allow an alert sound with completion notifications.",
        m_sound);
    auto* test = button("Test notification");
    notifications->addWidget(test, 0, Qt::AlignLeft);
    connect(test, &QPushButton::clicked, this, &SettingsPanel::notificationPreviewRequested);
    notifications->addWidget(
        label("Notifications contain no prompts, responses or document content.", "settingsHint"));
    auto* diagnostics = columns[6];
    m_diagnostics = new DiagnosticsSummary;
    m_diagnostics->setInstallHandlers([this](const QJsonObject& package) { installPackage(package); },
                                      [this] { cancelInstall(); });
    m_installPoll = new QTimer(this);
    m_installPoll->setInterval(1500);
    connect(m_installPoll, &QTimer::timeout, this, &SettingsPanel::pollInstall);
    diagnostics->addWidget(m_diagnostics);
    auto* refresh = button("Refresh details");
    diagnostics->addWidget(refresh, 0, Qt::AlignLeft);
    connect(refresh, &QPushButton::clicked, this, &SettingsPanel::refreshDetails);
    auto* report = button("Review report");
    row(diagnostics, "Diagnostic summary",
        "Review app and runtime details before saving a report. Personal paths and credentials are "
        "omitted.",
        report);
    connect(report, &QPushButton::clicked, this, &SettingsPanel::reviewReport);
    auto* logs = button("View logs");
    row(diagnostics, "Local logs",
        "Inspect backend messages locally. Logs may contain model names and personal file paths.",
        logs);
    connect(logs, &QPushButton::clicked, this, &SettingsPanel::viewLogs);
    auto* updates = columns[7];
    updates->addWidget(signature(true));
    updates->addWidget(label("Preview channel", "settingsHint"));
    m_updateStatus = label("Check for a new preview release.", "updateStatus");
    updates->addWidget(m_updateStatus);
    m_updateCheck = button("Check for updates");
    m_updateDownload = button("Download update", "primaryBtn");
    m_updateInstall = button("Restart to install", "primaryBtn");
    m_updateCancel = button("Cancel download");
    for (auto* control : {m_updateCheck, m_updateDownload, m_updateInstall, m_updateCancel}) {
        updates->addWidget(control, 0, Qt::AlignLeft);
        control->setEnabled(false);
    }
    auto* notes = button("Release notes");
    updates->addWidget(notes, 0, Qt::AlignLeft);
    connect(notes, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(
            QUrl("https://github.com/" + QStringLiteral(MAGNITE_RELEASE_REPOSITORY) + "/releases"));
    });
    m_automatic = new ToggleSwitch("", this);
    m_automatic->setAccessibleName("Check for updates on startup");
    row(updates, "Check on startup", "Look for new preview releases when Magnite Space opens.",
        m_automatic);
    note(updates, "Restart when your work is finished",
         "Updates replace the app and bundled runtime. Your models and application data stay in "
         "place. Installation is postponed while the backend has active work.");
    for (auto* column : columns)
        column->addStretch();
    m_pageEffect = new QGraphicsOpacityEffect(m_pages);
    m_pages->setGraphicsEffect(m_pageEffect);
    m_pageEffect->setOpacity(1);
    m_pageMotion = new MotionValue(this, [this](qreal value) { m_pageEffect->setOpacity(value); });
    m_pageMotion->snap(1);
    loadFromSettings();
    for (auto* combo : {m_theme, m_scale, m_sendKey, m_meterStyle, m_autoCompact})
        connect(combo, qOverload<int>(&QComboBox::currentIndexChanged), this,
                [this] { saveImmediate(); });
    for (auto* control :
         {m_grid, m_reducedMotion, m_startMaximized, m_trainingNotice, m_downloadNotice, m_sound, m_automatic})
        connect(control, &QCheckBox::toggled, this, [this] { saveImmediate(); });
    connect(m_profileName, &QLineEdit::editingFinished, this, [this] {
        Greeting::setUserName(m_profileName->text());
        m_profileName->setText(Greeting::userName());
        emit settingsChanged();
    });
    auto dirty = [this] {
        m_saveButton->setEnabled(true);
        m_storageSave->setEnabled(true);
        setStatus("You have unsaved runtime or storage changes.");
    };
    connect(m_dataDir, &QLineEdit::textChanged, this, dirty);
    connect(m_engineOverride, qOverload<int>(&QComboBox::currentIndexChanged), this, dirty);
    selectCategory(0);
}

void SettingsPanel::loadFromSettings() {
    m_loading = true;
    QSettings s;
    s.remove("hosted/apiKey");
    s.remove("hosted/endpoint");
    m_engineOverride->setCurrentIndex(qBound(0, s.value("engine/override", 0).toInt(), 2));
    m_dataDir->setText(s.value("storage/dataDir").toString());
    m_profileName->setText(Greeting::userName());
    m_grid->setChecked(s.value("appearance/studio3Ambient", true).toBool());
    m_reducedMotion->setChecked(s.value("appearance/studio3ReducedMotion", false).toBool());
    m_startMaximized->setChecked(s.value("appearance/startMaximized", true).toBool());
    m_theme->setCurrentIndex(qMax(
        0, m_theme->findData(s.value("appearance/themeMode", Theme::isDark() ? "dark" : "light"))));
    m_scale->setCurrentIndex(qBound(0, s.value("appearance/interfaceSize", 1).toInt(), 2));
    m_sendKey->setCurrentIndex(s.value("chat/sendKey", "enter") == "modified" ? 1 : 0);
    const auto style = ContextMeterLogic::styleKey(
        ContextMeterLogic::parseStyle(s.value("chat/meterStyle").toString()));
    m_meterStyle->setCurrentIndex(qMax(0, m_meterStyle->findData(style)));
    m_autoCompact->setCurrentIndex(
        qMax(0, m_autoCompact->findData(
                    ContextMeterLogic::parseAutoCompact(s.value("chat/autoCompactPercent")))));
    m_trainingNotice->setChecked(s.value("notifications/training", true).toBool());
    m_downloadNotice->setChecked(s.value("notifications/downloads", true).toBool());
    m_sound->setChecked(s.value("notifications/sound", false).toBool());
    m_automatic->setChecked(s.value("updates/checkOnStartup", true).toBool());
    m_loading = false;
    m_saveButton->setEnabled(false);
    m_storageSave->setEnabled(false);
}
void SettingsPanel::saveImmediate() {
    if (m_loading)
        return;
    QSettings s;
    s.setValue("appearance/themeMode", m_theme->currentData());
    s.setValue("appearance/interfaceSize", m_scale->currentIndex());
    s.setValue("appearance/studio3Ambient", m_grid->isChecked());
    s.setValue("appearance/studio3ReducedMotion", m_reducedMotion->isChecked());
    s.setValue("appearance/startMaximized", m_startMaximized->isChecked());
    s.setValue("chat/sendKey", m_sendKey->currentData());
    s.setValue("chat/meterStyle", m_meterStyle->currentData());
    s.setValue("chat/autoCompactPercent", m_autoCompact->currentData());
    s.setValue("notifications/training", m_trainingNotice->isChecked());
    s.setValue("notifications/downloads", m_downloadNotice->isChecked());
    s.setValue("notifications/sound", m_sound->isChecked());
    s.setValue("updates/checkOnStartup", m_automatic->isChecked());
    s.sync();
    if (MotionValue::reduced())
        m_pageMotion->snap(1);
    Theme::applyPreferences(true);
    refreshNavigation();
    setStatus(s.status() == QSettings::NoError
                  ? "Preferences saved on this device."
                  : "Could not save preferences. Check your settings file permissions.");
    emit settingsChanged();
}
void SettingsPanel::onSave() {
    const QString path = m_dataDir->text().trimmed();
    if (!path.isEmpty() &&
        (!QDir::isAbsolutePath(path) || (QFileInfo::exists(path) && !QFileInfo(path).isDir()))) {
        setStatus("Choose an absolute directory path, or leave it empty for the default.");
        m_dataDir->setFocus();
        return;
    }
    QSettings s;
    s.setValue("engine/override", m_engineOverride->currentIndex());
    s.setValue("storage/dataDir", path);
    s.sync();
    if (s.status() != QSettings::NoError) {
        setStatus("Could not save runtime and storage preferences.");
        return;
    }
    m_saveButton->setEnabled(false);
    m_storageSave->setEnabled(false);
    setStatus("Saved. Storage changes apply after restart; existing data is not moved.");
    emit settingsChanged();
}
void SettingsPanel::onBrowseDataDir() {
    const QString path = QFileDialog::getExistingDirectory(
        this, "Select data directory",
        m_dataDir->text().isEmpty() ? m_activeDataPath : m_dataDir->text());
    if (!path.isEmpty())
        m_dataDir->setText(path);
}
void SettingsPanel::setStatus(const QString& text) {
    m_statusLabel->setText(text);
    m_statusLabel->setAccessibleDescription(text);
}
void SettingsPanel::selectCategory(int index) {
    if (index < 0 || index >= titles.size())
        return;
    const bool change = index != m_pages->currentIndex();
    m_pages->setCurrentIndex(index);
    m_title->setText(titles[index]);
    refreshNavigation();
    if (change && isVisible()) {
        m_pageMotion->snap(.86);
        m_pageMotion->to(1, true, Theme::motionFastMs());
    }
    if (index == 4 || index == 6)
        refreshDetails();
    if (index == kProfilePage)
        refreshProfile();
}
void SettingsPanel::refreshNavigation() {
    for (int i = 0; i < 3; ++i)
        if (auto* choice = findChild<QPushButton*>("themeChoice" + QString::number(i)))
            choice->setChecked(i == m_theme->currentIndex());
    for (int i = 0; i < m_navigation.size(); ++i) {
        auto* item = m_navigation[i];
        item->setChecked(i == m_pages->currentIndex());
        item->setIcon(Theme::coloredIcon(
            icons[i],
            QColor(i == m_pages->currentIndex() ? Theme::accentLabel() : Theme::textSecondary()),
            17));
    }
}
void SettingsPanel::showEvent(QShowEvent* event) {
    BasePanel::showEvent(event);
    if (!m_profileName->hasFocus())
        m_profileName->setText(Greeting::userName());
    {
        QSignalBlocker block(m_theme);
        m_theme->setCurrentIndex(
            qMax(0, m_theme->findData(QSettings().value("appearance/themeMode",
                                                        Theme::isDark() ? "dark" : "light"))));
    }
    refreshNavigation();
    refreshDetails();
}
void SettingsPanel::setBackendClient(BackendClient* client) {
    m_client = client;
}
void SettingsPanel::setChatContext(ChatStore* store, ChatPanel* chat) {
    m_store = store;
    m_chat = chat;
    auto update = [this] {
        m_chatCount->setText(QString::number(m_store->list().size()) + " saved conversations");
        m_deleteChats->setEnabled(!m_chat->isBusy() && !m_store->list().isEmpty());
    };
    connect(store, &ChatStore::changed, this, update);
    connect(chat, &ChatPanel::busyChanged, this, [update](bool) { update(); });
    update();
    m_activeDataPath = QFileInfo(store->directory()).absolutePath();
    m_effectivePath->setText(m_activeDataPath);
}
void SettingsPanel::refreshDetails() {
    if (!m_client || !m_client->isReady()) {
        m_info = {};
        m_diagnostics->setUnavailable(
            "Local backend not connected. Details will be available once it is ready.");
        return;
    }
    if (m_detailsPending)
        return;
    m_detailsPending = true;
    m_client->get("/system/info", [this](int status, const QJsonDocument& doc) {
        if (status != 200 || !doc.isObject()) {
            m_info = {};
            m_diagnostics->setUnavailable(
                "Could not read local runtime details. Check the connection and retry.");
            return;
        }
        m_info = doc.object();
        m_diagnostics->setDetails(m_info);
    });
    refreshPackages();
    refreshPartials();
    m_client->get("/system/storage", [this](int status, const QJsonDocument& doc) {
        m_detailsPending = false;
        if (status != 200 || !doc.isObject()) {
            m_storageInfo->setUnavailable(
                "Could not measure storage usage. Refresh details to retry.");
            return;
        }
        const auto o = doc.object();
        m_cacheDirectory = o["model_cache"].toString();
        m_cachePath->setText(m_cacheDirectory +
                             "\nShared with other apps; may be outside Magnite Space's data directory.");
        m_activeDataPath = o["data_directory"].toString();
        m_effectivePath->setText(m_activeDataPath);
        m_storageInfo->setMeasurements(o);
    });
}
void SettingsPanel::refreshProfile() {
    m_profileAvatar->setPixmap(Theme::coloredIcon("user", QColor(Theme::accentLabel()), 26).pixmap(26, 26));
    const QString name = Greeting::userName();
    m_profileTitle->setText(name.isEmpty() ? "Your workspace" : name);
    const auto count = [](qint64 n) { return QLocale().toString(n); };
    const auto set = [this](const QString& key, const QString& value, const QString& detail) {
        m_statValues[key]->setText(value);
        m_statDetails[key]->setText(detail.isEmpty() ? " " : detail);
    };
    if (m_store)
        set("conversations", count(m_store->list().size()), "Saved chats on this computer");
    if (!m_client || !m_client->isReady()) {
        m_profileStatus->setText("Connect to the local service to count models, runs and documents.");
        return;
    }
    if (m_profileBusy)
        return;
    m_profileBusy = true;
    QPointer<SettingsPanel> guard(this);
    m_client->get("/system/stats", [guard, count, set](int status, const QJsonDocument& doc) {
        if (!guard)
            return;
        guard->m_profileBusy = false;
        if (status != 200 || !doc.isObject()) {
            guard->m_profileStatus->setText(QString("Could not read workspace statistics (HTTP %1).").arg(status));
            return;
        }
        const auto o = doc.object();
        const QLocale locale;
        const auto plural = [](qint64 n, const QString& word) { return word + (n == 1 ? "" : "s"); };
        const auto section = [&o](const char* key) { return o[key].toObject(); };
        const auto missing = [&o](const char* key) { return !o[key].isObject(); };
        auto num = [](const QJsonObject& s, const char* key) { return s[key].toVariant().toLongLong(); };
        QStringList unavailable;
        if (missing("models")) unavailable << "models";
        else {
            const auto m = section("models");
            QStringList parts;
            if (num(m, "downloaded")) parts << QString("%1 downloaded").arg(count(num(m, "downloaded")));
            if (num(m, "imported")) parts << QString("%1 imported").arg(count(num(m, "imported")));
            if (m["size_mb"].toDouble() > 0)
                parts << locale.formattedDataSize(qint64(m["size_mb"].toDouble() * 1024 * 1024), 1,
                                                  QLocale::DataSizeTraditionalFormat);
            set("models", count(num(m, "total")), parts.join(" · "));
        }
        if (missing("adapters")) unavailable << "adapters";
        else {
            const auto a = section("adapters");
            set("adapters", count(num(a, "total")),
                num(a, "total") ? QString("%1 trained here").arg(count(num(a, "trained"))) : QString());
        }
        if (missing("training")) unavailable << "training";
        else {
            const auto t = section("training");
            QStringList parts;
            if (num(t, "runs")) parts << QString("%1 of %2 completed").arg(count(num(t, "completed")), count(num(t, "runs")));
            if (num(t, "steps")) parts << QString("%1 %2").arg(count(num(t, "steps")), plural(num(t, "steps"), "step"));
            set("training", count(num(t, "runs")), parts.join(" · "));
        }
        if (missing("datasets")) unavailable << "datasets";
        else {
            const auto d = section("datasets");
            set("datasets", count(num(d, "total")),
                num(d, "rows") ? QString("%1 %2").arg(count(num(d, "rows")), plural(num(d, "rows"), "example")) : QString());
        }
        if (missing("knowledge")) unavailable << "Knowledge";
        else {
            const auto k = section("knowledge");
            set("knowledge", count(num(k, "documents")),
                num(k, "chunks") ? QString("%1 indexed %2").arg(count(num(k, "chunks")), plural(num(k, "chunks"), "passage")) : QString());
        }
        if (!Features::blueprintsAndTools) {
            // This build has no Blueprints or Tool mode, so there is nothing to count.
        } else if (missing("blueprints")) unavailable << "Blueprints";
        else {
            const auto b = section("blueprints");
            set("blueprints", count(num(b, "blueprints")),
                num(b, "runs") ? QString("%1 workflow %2").arg(count(num(b, "runs")), plural(num(b, "runs"), "run")) : QString());
            set("tasks", count(num(b, "tool_tasks")), "Tasks in Tool mode");
        }
        const auto since = QDate::fromString(o["since"].toString().left(10), Qt::ISODate);
        guard->m_profileSince->setText(since.isValid()
            ? QString("In Magnite Space since %1 · %2").arg(locale.toString(since, "MMMM yyyy"), QCoreApplication::applicationVersion())
            : QString("A fresh workspace · %1").arg(QCoreApplication::applicationVersion()));
        guard->m_profileStatus->setText(unavailable.isEmpty()
            ? "Counts come from data stored on this computer. Nothing leaves your device."
            : "Not available right now: " + unavailable.join(", ") + ". Install the optional ML features or retry.");
    });
}
void SettingsPanel::refreshPartials() {
    if (!m_client || !m_client->isReady() || m_partialBusy)
        return;
    m_partialBusy = true;
    m_partialScan->setEnabled(false);
    m_partialSummary->setText("Scanning the model cache…");
    QPointer<SettingsPanel> guard(this);
    m_client->get("/models/partials", [guard](int status, const QJsonDocument& doc) {
        if (!guard)
            return;
        guard->m_partialBusy = false;
        guard->m_partialScan->setEnabled(true);
        guard->m_partials.clear();
        if (status != 200 || !doc.isArray()) {
            guard->m_partialLoaded = false;
            const QString detail = doc.object()["detail"].toString();
            guard->m_partialSummary->setText(detail.isEmpty()
                ? QString("Could not scan for partial downloads (HTTP %1).").arg(status)
                : "Could not scan for partial downloads: " + detail);
            guard->renderPartials();
            return;
        }
        guard->m_partialLoaded = true;
        for (const auto& entry : doc.array())
            guard->m_partials << entry.toObject();
        guard->renderPartials();
    });
}
void SettingsPanel::renderPartials() {
    auto* body = qobject_cast<QVBoxLayout*>(m_partialHost->layout());
    while (auto* item = body->takeAt(0)) {
        if (auto* widget = item->widget())
            widget->deleteLater();
        delete item;
    }
    m_partialDeleteAll->setVisible(m_partials.size() > 1);
    m_partialDeleteAll->setEnabled(!m_partialBusy);
    if (!m_partialLoaded)
        return;
    qint64 total = 0;
    for (const auto& partial : m_partials)
        total += partial["bytes"].toVariant().toLongLong();
    const QLocale locale;
    if (m_partials.isEmpty()) {
        m_partialSummary->setText(
            "No unfinished downloads. Interrupted or failed Hub downloads appear here.");
        return;
    }
    m_partialSummary->setText(
        QString("%1 unfinished download%2 using %3. A retry from the Hub resumes from these "
                "files; delete them to free the space.")
            .arg(m_partials.size())
            .arg(m_partials.size() == 1 ? "" : "s")
            .arg(locale.formattedDataSize(total, 1, QLocale::DataSizeTraditionalFormat)));
    for (const auto& partial : m_partials) {
        const QString repo = partial["repo"].toString();
        auto* host = new QWidget(m_partialHost);
        host->setObjectName("settingsRow");
        auto* rowBody = new QHBoxLayout(host);
        rowBody->setContentsMargins(0, 10, 0, 10);
        rowBody->setSpacing(16);
        auto* copy = new QVBoxLayout;
        copy->setSpacing(4);
        auto* title = label(repo, "settingsRowTitle", host);
        title->setTextInteractionFlags(Qt::TextSelectableByMouse);
        copy->addWidget(title);
        QStringList facts{
            locale.formattedDataSize(partial["bytes"].toVariant().toLongLong(), 1,
                                     QLocale::DataSizeTraditionalFormat),
            QString("%1 unfinished file%2").arg(partial["files"].toInt())
                .arg(partial["files"].toInt() == 1 ? "" : "s")};
        const auto modified =
            QDateTime::fromString(partial["modified"].toString(), Qt::ISODate).toLocalTime();
        if (modified.isValid())
            facts << "last written " + locale.toString(modified, QLocale::ShortFormat);
        if (partial["has_complete_files"].toBool())
            facts << "finished files are kept";
        copy->addWidget(label(facts.join(" · "), "settingsHint", host));
        rowBody->addLayout(copy, 1);
        auto* remove = button("Delete", "destructiveBtn");
        remove->setParent(host);
        remove->setAccessibleName("Delete partial download " + repo);
        remove->setEnabled(!m_partialBusy);
        connect(remove, &QPushButton::clicked, this, [this, repo] { deletePartials({repo}); });
        rowBody->addWidget(remove, 0, Qt::AlignVCenter);
        body->addWidget(host);
    }
}
void SettingsPanel::deletePartials(const QStringList& repos) {
    if (repos.isEmpty() || m_partialBusy || !m_client || !m_client->isReady())
        return;
    qint64 total = 0;
    for (const auto& partial : m_partials)
        if (repos.contains(partial["repo"].toString()))
            total += partial["bytes"].toVariant().toLongLong();
    const QString size =
        QLocale().formattedDataSize(total, 1, QLocale::DataSizeTraditionalFormat);
    const QString message = repos.size() == 1
        ? QString("Delete the unfinished download of “%1”?\n\nThis frees about %2. Finished "
                  "files from earlier downloads stay. Downloading it again starts from the "
                  "beginning.").arg(repos.first(), size)
        : QString("Delete %1 unfinished downloads?\n\nThis frees about %2. Finished files from "
                  "earlier downloads stay.").arg(repos.size()).arg(size);
    if (!Ui::confirmRemoval(this, "Delete partial downloads?", message))
        return;
    m_partialBusy = true;
    m_partialScan->setEnabled(false);
    renderPartials();
    setStatus("Deleting partial downloads…");
    auto remaining = std::make_shared<int>(repos.size());
    auto failures = std::make_shared<QStringList>();
    auto freed = std::make_shared<qint64>(0);
    QPointer<SettingsPanel> guard(this);
    for (const QString& repo : repos) {
        m_client->remove("/models/partials?repo=" + QString::fromUtf8(QUrl::toPercentEncoding(repo)),
                         [guard, repo, remaining, failures, freed](int status, const QJsonDocument& doc) {
            if (!guard)
                return;
            if (status == 200)
                *freed += doc.object()["bytes_freed"].toVariant().toLongLong();
            else if (status != 404) {
                const QString detail = doc.object()["detail"].toString();
                *failures << repo + (detail.isEmpty() ? QString(" (HTTP %1)").arg(status) : ": " + detail);
            }
            if (--*remaining > 0)
                return;
            guard->m_partialBusy = false;
            const QString size =
                QLocale().formattedDataSize(*freed, 1, QLocale::DataSizeTraditionalFormat);
            guard->setStatus(failures->isEmpty()
                ? QString("Partial downloads deleted. Freed about %1.").arg(size)
                : "Some partial downloads could not be deleted: " + failures->join("; "));
            guard->refreshPartials();
            guard->refreshDetails();   // storage totals changed
        });
    }
}
void SettingsPanel::refreshPackages() {
    if (!m_client || !m_client->isReady())
        return;
    m_client->get("/system/packages", [this](int status, const QJsonDocument& doc) {
        if (status == 200 && doc.isObject())
            m_diagnostics->setPackages(doc.object()["packages"].toArray());
    });
    m_client->get("/system/packages/install", [this](int status, const QJsonDocument& doc) {
        if (status != 200 || !doc.isObject())
            return;
        m_diagnostics->setInstallJob(doc.object());
        if (doc.object()["state"].toString() == "running")
            m_installPoll->start(); // an install started earlier is still going
    });
}
void SettingsPanel::installPackage(const QJsonObject& package) {
    if (!m_client || !m_client->isReady())
        return;
    const QString label = package["label"].toString(), id = package["id"].toString();
    if (!Ui::confirmAction(this, "Install " + label + "?",
                           label + " will be downloaded from the network (" +
                               package["size_hint"].toString().toLower() +
                               ") and added to Magnite Space's own packages folder. Your models, chats and "
                               "the app itself are not changed.",
                           "Install"))
        return;
    m_client->post("/system/packages/" + QString::fromUtf8(QUrl::toPercentEncoding(id)) + "/install",
                   QJsonObject{}, [this, label](int status, const QJsonDocument& doc) {
                       if (status != 202 || !doc.isObject()) {
                           setStatus(doc.object()["detail"].toString("Could not start the install."));
                           return;
                       }
                       m_diagnostics->setInstallJob(doc.object());
                       m_installPoll->start();
                       setStatus("Installing " + label + "…");
                   });
}
void SettingsPanel::cancelInstall() {
    if (!m_client || !m_client->isReady())
        return;
    m_client->post("/system/packages/install/cancel", QJsonObject{},
                   [this](int, const QJsonDocument&) { pollInstall(); });
}
void SettingsPanel::pollInstall() {
    if (!m_client || !m_client->isReady() || m_installPolling)
        return;
    m_installPolling = true;
    m_client->get("/system/packages/install", [this](int status, const QJsonDocument& doc) {
        m_installPolling = false;
        if (status != 200 || !doc.isObject())
            return;
        const auto job = doc.object();
        const QString state = job["state"].toString();
        m_diagnostics->setInstallJob(job);
        if (state == "running")
            return;
        m_installPoll->stop();
        refreshPackages();
        if (state == "done")
            setStatus("Installed " + job["id"].toString() + ". It is ready to use.");
        else if (state == "failed")
            setStatus("Install failed. The details are shown on the package.");
        else if (state == "cancelled")
            setStatus("Install cancelled.");
    });
}
void SettingsPanel::resetPreferences() {
    if (!Ui::confirmRemoval(
            this, "Reset preferences?",
            "Restore appearance, chat, notifications, runtime and startup-update preferences? Your "
            "name, data directory and saved content stay in place.",
            "Reset preferences"))
        return;
    QSettings s;
    for (const auto& key :
         {"appearance/themeMode", "appearance/interfaceSize", "appearance/studio3Ambient",
          "appearance/studio3ReducedMotion", "appearance/startMaximized", "chat/sendKey", "chat/meterStyle",
          "chat/autoCompactPercent", "notifications/training", "notifications/downloads",
          "notifications/sound", "engine/override", "updates/checkOnStartup"})
        s.remove(key);
    s.setValue("appearance/themeMode", "dark");
    s.sync();
    loadFromSettings();
    Theme::applyPreferences();
    refreshNavigation();
    m_pageMotion->snap(1);
    emit settingsChanged();
    setStatus("Preferences reset. Saved content and the data directory were preserved.");
}
bool SettingsPanel::writeJson(const QString& title, const QJsonObject& object) {
    const QString path =
        QFileDialog::getSaveFileName(this, title, QString(), "JSON files (*.json)");
    if (path.isEmpty())
        return false;
    QSaveFile file(path);
    const auto data = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit()) {
        setStatus("Could not save the export: " + file.errorString());
        return false;
    }
    setStatus("Export saved to " + path);
    return true;
}
void SettingsPanel::exportChats() {
    if (!m_store || !m_chat)
        return;
    if (!m_chat->flushSave()) {
        setStatus("Could not save the current conversation. Export cancelled.");
        return;
    }
    QJsonObject object;
    QString error;
    if (!m_store->exportAll(&object, &error)) {
        setStatus("Export cancelled: " + error);
        return;
    }
    writeJson("Export all chats", object);
}
void SettingsPanel::deleteChats() {
    if (!m_store || !m_chat)
        return;
    if (m_chat->isBusy()) {
        setStatus("Wait for the current response or compaction to finish before deleting chats.");
        return;
    }
    if (!Ui::confirmRemoval(this, "Delete all chats?",
                            QString("Permanently delete %1 saved conversations? This cannot be "
                                    "undone. Export your chats first if you want a copy. Models, "
                                    "documents and training runs stay in place.")
                                .arg(m_store->list().size()),
                            "Delete all chats"))
        return;
    if (m_chat->isBusy())
        return;
    const auto chats = m_store->list();
    int removed = 0;
    for (const auto& chat : chats) {
        if (!m_chat->deleteChat(chat.id)) {
            setStatus(QString("Deleted %1 chats; could not delete the rest: %2")
                          .arg(removed)
                          .arg(m_store->lastError()));
            return;
        }
        ++removed;
    }
    setStatus(QString("Deleted %1 saved conversations.").arg(removed));
}
void SettingsPanel::showText(const QString& title, const QString& text, bool exportable) {
    SettingsDetailDialog dialog(this);
    dialog.setWindowTitle(title);
    dialog.resize(620, 430);
    auto* body = new QVBoxLayout(&dialog);
    body->setContentsMargins(24, 22, 24, 22);
    auto* heading = new QHBoxLayout;
    auto *dragTitle = label(title, "settingsTitle");
    dragTitle->setProperty("windowDragRegion", true);
    heading->addWidget(dragTitle, 1);
    heading->addWidget(Ui::dialogCloseButton(&dialog));
    body->addLayout(heading);
    auto* filter = new QLineEdit;
    filter->setPlaceholderText("Filter lines…");
    filter->setAccessibleName("Filter report or log lines");
    body->addWidget(filter);
    auto* view = new QPlainTextEdit;
    view->setReadOnly(true);
    view->setPlainText(text);
    body->addWidget(view, 1);
    connect(filter, &QLineEdit::textChanged, &dialog, [view, text](const QString& query) {
        QStringList lines;
        for (const auto& line : text.split('\n'))
            if (query.isEmpty() || line.contains(query, Qt::CaseInsensitive))
                lines << line;
        view->setPlainText(lines.join('\n'));
    });
    auto* actions = new QHBoxLayout;
    auto* copy = button("Copy visible text");
    actions->addWidget(copy);
    connect(copy, &QPushButton::clicked, &dialog,
            [view] { QApplication::clipboard()->setText(view->toPlainText()); });
    actions->addStretch();
    if (exportable) {
        auto* save = button("Save report", "primaryBtn");
        actions->addWidget(save);
        connect(save, &QPushButton::clicked, &dialog, [this, text] {
            writeJson("Save diagnostic report", QJsonDocument::fromJson(text.toUtf8()).object());
        });
    }
    auto* close = button("Close");
    actions->addWidget(close);
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    body->addLayout(actions);
    dialog.exec();
}
void SettingsPanel::reviewReport() {
    if (m_info.isEmpty()) {
        setStatus("Refresh diagnostics before creating a report.");
        return;
    }
    // Deliberately select fields. Device descriptions, paths, logs and model ids are not exported.
    QJsonObject report{{"application", QCoreApplication::applicationVersion()},
                       {"backend", m_info["version"]},
                       {"system", m_info["system"]},
                       {"architecture", m_info["architecture"]},
                       {"python", m_info["python"]},
                       {"engines", m_info["engines"]}};
    const auto device = m_info["device"].toObject();
    report["memory_kind"] = device["kind"];
    report["memory_total_bytes"] = device["total_bytes"];
    report["accelerator"] = device["accelerator"];
    showText("Review diagnostic report",
             QString::fromUtf8(QJsonDocument(report).toJson(QJsonDocument::Indented)), true);
}
void SettingsPanel::viewLogs() {
    const QString desktop =
        m_runtimeLogs ? m_runtimeLogs() : QString("No desktop runtime messages available.");
    if (!m_client || !m_client->isReady()) {
        showText("Local logs", desktop);
        return;
    }
    m_client->get("/system/logs", [this, desktop](int status, const QJsonDocument& doc) {
        if (!isVisible())
            return;
        showText("Local logs", desktop + "\n\n— vLLM runtime —\n" +
                                   (status == 200 ? doc.object()["text"].toString()
                                                  : "Could not read the vLLM log."));
    });
}
void SettingsPanel::setUpdateManager(UpdateManager* manager) {
    m_updater = manager;
    connect(m_updateCheck, &QPushButton::clicked, manager, &UpdateManager::check);
    connect(m_updateDownload, &QPushButton::clicked, manager, &UpdateManager::download);
    connect(m_updateInstall, &QPushButton::clicked, manager, &UpdateManager::installRequested);
    connect(m_updateCancel, &QPushButton::clicked, manager, &UpdateManager::cancel);
    auto refresh = [this, manager] {
        const auto state = manager->state();
        m_updateStatus->setText(manager->status());
        m_updateCheck->setEnabled(state == UpdateManager::Idle ||
                                  state == UpdateManager::Available ||
                                  state == UpdateManager::Failed);
        m_updateDownload->setVisible(state == UpdateManager::Available);
        m_updateDownload->setEnabled(state == UpdateManager::Available && manager->canInstall());
        m_updateInstall->setVisible(state == UpdateManager::Ready);
        m_updateInstall->setEnabled(state == UpdateManager::Ready && manager->canInstall());
        m_updateCancel->setVisible(state == UpdateManager::Downloading);
        m_updateCancel->setEnabled(state == UpdateManager::Downloading);
    };
    connect(manager, &UpdateManager::changed, this, refresh);
    refresh();
}

void SettingsPanel::changeEvent(QEvent* event) {
    BasePanel::changeEvent(event);
    if (event->type() == QEvent::ApplicationPaletteChange || event->type() == QEvent::StyleChange) {
        refreshNavigation();
        if (m_profileAvatar)
            m_profileAvatar->setPixmap(Theme::coloredIcon("user", QColor(Theme::accentLabel()), 26).pixmap(26, 26));
    }
}

void SettingsPanel::showShortcuts() {
    SettingsDetailDialog dialog(this);
    dialog.setWindowTitle("Keyboard shortcuts");
    dialog.resize(600, 540);
    auto* body = new QVBoxLayout(&dialog);
    body->setContentsMargins(28, 24, 28, 24);
    body->setSpacing(14);
    auto* heading = new QHBoxLayout;
    auto* copy = new QVBoxLayout;
    copy->addWidget(label("QUICK REFERENCE", "settingsEyebrow"));
    auto *dragTitle = label("Keyboard shortcuts", "settingsTitle");
    dragTitle->setProperty("windowDragRegion", true);
    copy->addWidget(dragTitle);
    heading->addLayout(copy, 1);
    heading->addWidget(Ui::dialogCloseButton(&dialog), 0, Qt::AlignTop);
    body->addLayout(heading);
    const QList<QPair<QString, QString>> shortcuts = {
        {"Open settings", Shortcuts::label(Shortcuts::settings())},
        {"New chat", Shortcuts::label(Shortcuts::newChat())},
        {"Switch workspace", Shortcuts::label(Shortcuts::workspace(0)) + " … " +
                                 Shortcuts::label(Shortcuts::workspace(5))},
        {"Open settings from sidebar", Shortcuts::label(Shortcuts::workspace(6))},
        {"Collapse sidebar", Shortcuts::label(Shortcuts::collapseSidebar())},
        {"Close a dialog", Shortcuts::label(QKeySequence(Qt::Key_Escape))},
        {"New line in a message", Shortcuts::label(Shortcuts::newLine())}};
    for (const auto& shortcut : shortcuts) {
        auto* row = new QHBoxLayout;
        row->addWidget(label(shortcut.first, "settingsRowTitle"), 1);
        auto* key = label(shortcut.second, "settingsKeycap");
        key->setAlignment(Qt::AlignCenter);
        row->addWidget(key);
        body->addLayout(row);
        line(body);
    }
    body->addStretch();
    body->addWidget(
        label("Your Chat preferences control which shortcut sends a message.", "settingsHint"));
    dialog.exec();
}
