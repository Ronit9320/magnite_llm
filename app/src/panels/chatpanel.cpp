#include "widgets/spinbox.h"
#include <QPlainTextEdit>
#include <QTime>
#include <QRandomGenerator>
#include <QKeyEvent>
#include <QTextCursor>
#include "panels/chatpanel.h"
#include "panels/blueprintpanel.h"
#include "panels/chatstore.h"
#include "panels/ragpanel.h"
#include "theme.h"
#include "widgets/edgescrollbar.h"
#include "panels/greeting.h"

#include "backend_client.h"
#include "widgets/surface.h"
#include "widgets/studiocanvas.h"
#include "widgets/brandmark.h"
#include <QPainter>
#include "widgets/contextmeter.h"
#include "widgets/librarydialog.h"
#include "widgets/flowlayout.h"
#include "panels/chatbubble.h"
#include "panels/messagerow.h"

#include <QSettings>
#include "widgets/shortcuts.h"
#include <QStyle>
#include "widgets/toggleswitch.h"
#include "widgets/combobox.h"
#include "widgets/utilizationmeter.h"
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QNetworkReply>
#include <QLineEdit>
#include <QPushButton>
#include "widgets/button.h"
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QShowEvent>
#include <QSpinBox>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>

namespace {
class ConversationLogo : public QWidget {
public:
    using QWidget::QWidget;
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        BrandMark::paint(painter, rect(), QColor(Theme::accentPrimary()));
    }
};

QPushButton *toolbarIcon(QWidget *parent, const QString &label, const QString &icon) {
    auto *button = new QPushButton(parent);
    button->setProperty("conversationIcon", true);
    button->setProperty("themeIcon", icon);
    button->setIcon(Theme::coloredIcon(icon, QColor(Theme::textSecondary()), 18));
    button->setIconSize(QSize(18, 18));
    button->setFixedSize(34, 34);
    button->setAccessibleName(label);
    button->setToolTip(label);
    button->setCursor(Qt::PointingHandCursor);
    return button;
}
}

ChatPanel::ChatPanel(QWidget *parent)
    : BasePanel("Conversation", parent, true)
    , m_client(nullptr)
    , m_modelCombo(nullptr)
    , m_engineCombo(nullptr)
    , m_maxTokensSpin(nullptr)
    , m_tempSpin(nullptr)
    , m_ragCheck(nullptr)
    , m_enginePill(nullptr)
    , m_modelPill(nullptr)
    , m_adapterPill(nullptr)
    , m_chatScroll(nullptr)
    , m_chatHost(nullptr)
    , m_chatLayout(nullptr)
    , m_input(nullptr)
    , m_sendButton(nullptr)
    , m_stopButton(nullptr)
    , m_statusLabel(nullptr)
    , m_streamBubble(nullptr)
    , m_streaming(false)
    , m_cursorTimer(nullptr)
    , m_cursorVisible(false)
{
    setupUi();
    showBlankChat();
}

void ChatPanel::showBlankChat() {
    const int hour = m_hourProvider ? m_hourProvider() : QTime::currentTime().hour();
    m_greetingName = Greeting::userName();
    m_greeting = Greeting::pick(Greeting::variants(hour, m_greetingName), m_greeting, int(QRandomGenerator::global()->generate() & 0x7fffffff));
    m_emptyConversation->setText(m_greeting);
}

void ChatPanel::setupUi() {
    m_chatContents = new QStackedWidget(this);
    auto *regularChat = new QWidget;
    m_chatContents->addWidget(regularChat);
    contentLayout()->addWidget(m_chatContents, 1);
    auto *layout = new QVBoxLayout(regularChat);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(contentLayout()->spacing());
    auto *toolbar = new QHBoxLayout();
    toolbar->setSpacing(6);
    m_modelCombo = new ComboBox(this);
    m_modelCombo->setObjectName("conversationModelPicker");
    m_modelCombo->setEditable(true);
    m_modelCombo->setInsertPolicy(QComboBox::NoInsert);
    m_modelCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_modelCombo->setFixedWidth(205);
    m_modelCombo->setAccessibleName("Model");
    m_modelCombo->lineEdit()->setAccessibleName("Model ID");
    m_modelCombo->lineEdit()->setPlaceholderText("Select model");
    connect(m_modelCombo, &QComboBox::currentTextChanged, this, [this](const QString &text) {
        m_modelCombo->setToolTip(text.isEmpty() ? "Choose a model or enter its ID" : text);
    });
    m_modelCombo->setToolTip("Choose a model or enter its ID");
    toolbar->addWidget(m_modelCombo);
    m_modelButton = new QPushButton("Load model", this);
    m_modelButton->setObjectName("conversationModelAction");
    m_modelButton->setMinimumWidth(105);
    m_modelButton->setCursor(Qt::PointingHandCursor);
    toolbar->addWidget(m_modelButton);
    toolbar->addStretch();
    m_utilization = new UtilizationMeter(12, 2, this);
    m_utilization->setFixedWidth(106);
    m_utilizationLabel = new QLabel(this);
    m_utilizationLabel->setObjectName("conversationUtilization");
    m_utilizationLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_utilizationLabel->setMinimumWidth(75);
    applyUtilization({});
    toolbar->addWidget(m_utilizationLabel);
    toolbar->addWidget(m_utilization);
    toolbar->addSpacing(6);
    m_settingsToggle = toolbarIcon(this, "Model settings", "settings");
    m_settingsToggle->setObjectName("chatSettingsToggle");
    m_settingsToggle->setCheckable(true);
    toolbar->addWidget(m_settingsToggle);
    m_knowledgeToggle = toolbarIcon(this, "Knowledge", "database");
    m_knowledgeToggle->setObjectName("conversationKnowledgeToggle");
    m_knowledgeToggle->setCheckable(true);
    toolbar->addWidget(m_knowledgeToggle);
    m_knowledgeToggle->hide();
    layout->addLayout(toolbar);

    connect(m_settingsToggle, &QPushButton::toggled, this, [this](bool open) {
        if (open) m_knowledgeToggle->setChecked(false);
    });
    connect(m_knowledgeToggle, &QPushButton::toggled, this, [this](bool open) {
        if (open) m_settingsToggle->setChecked(false);
    });

    m_workspace = new QHBoxLayout;
    m_workspace->setSpacing(12);
    layout->addLayout(m_workspace, 1);
    auto *controls = addDrawer("Model settings", "chatSettings", m_settingsToggle);
    m_engineCombo = new ComboBox(this);
    m_engineCombo->addItems({"Auto (global setting)", "Local (vLLM)", "Local (llama.cpp)", "Local (torch)", "Local (Ollama)"});
    Surface::field(controls, "Engine", m_engineCombo);
    m_maxTokensSpin = new SpinBox(this);
    m_maxTokensSpin->setRange(1, 8192);
    m_maxTokensSpin->setValue(256);
    Surface::field(controls, "Response tokens", m_maxTokensSpin);
    m_tempSpin = new DoubleSpinBox(this);
    m_tempSpin->setRange(0.0, 2.0);
    m_tempSpin->setValue(0.7);
    m_tempSpin->setSingleStep(0.05);
    m_tempSpin->setDecimals(2);
    Surface::field(controls, "Temperature", m_tempSpin);
    controls->addSpacing(8);
    m_enginePill = new QLabel("engine: —", this);
    m_modelPill = new QLabel("model: —", this);
    m_adapterPill = new QLabel("adapter: —", this);
    for (auto *pill : {m_enginePill, m_modelPill, m_adapterPill}) {
        pill->setObjectName("chatPill");
        pill->setTextFormat(Qt::PlainText);
        pill->setWordWrap(true);
        controls->addWidget(pill);
    }
    m_adapterPill->hide();
    auto *refreshBtn = new Button("Refresh", this);
    controls->addWidget(refreshBtn, 0, Qt::AlignLeft);
    controls->addStretch();
    connect(refreshBtn, &QPushButton::clicked, this, [this]() { refreshModels(); refreshEngineStatus(); });
    connect(m_modelButton, &QPushButton::clicked, this, [this] {
        if (m_modelLoaded) onUnloadModel();
        else onLoadModel();
    });
    auto *telemetryTimer = new QTimer(this);
    connect(telemetryTimer, &QTimer::timeout, this, &ChatPanel::refreshUtilization);
    telemetryTimer->start(3000);

    // A readable, centered conversation column, without a permanent inspector.
    auto *centerRow = new QHBoxLayout();
    auto *column = new QWidget(this);
    column->setMaximumWidth(840);
    auto *conversation = new QVBoxLayout(column);
    conversation->setContentsMargins(12, 0, 12, 0);
    conversation->setSpacing(12);
    centerRow->addStretch();
    centerRow->addWidget(column, 1);
    centerRow->addStretch();
    m_workspace->insertLayout(0, centerRow, 1);
    m_chatScroll = new QScrollArea(this);
    m_chatScroll->setWidgetResizable(true);
    m_chatScroll->setFrameShape(QFrame::NoFrame);
    m_chatScroll->viewport()->setAutoFillBackground(false);
    m_chatScroll->setMinimumHeight(180);
    connect(m_chatScroll->verticalScrollBar(),&QScrollBar::valueChanged,this,[this](int value) {
        m_followOutput = value >= m_chatScroll->verticalScrollBar()->maximum()-24;
    });
    m_chatHost = new QWidget(this);
    m_chatLayout = new QVBoxLayout(m_chatHost);
    m_chatLayout->setContentsMargins(4, 8, 12, 8);
    m_chatLayout->setSpacing(14);
    m_welcome = new QWidget(m_chatHost);
    auto *welcome = new QVBoxLayout(m_welcome);
    welcome->setSpacing(8);
    welcome->setContentsMargins(0,4,0,4);
    welcome->addStretch();
    auto *seal = new ConversationLogo(m_welcome);
    seal->setObjectName("conversationLogo");
    seal->setFixedSize(64, 64);
    seal->setAccessibleName("Magnite Space logo");
    welcome->addWidget(seal, 0, Qt::AlignHCenter);
    auto *eyebrow = new QLabel("THE MAGNITE WORKSPACE", m_welcome);
    eyebrow->setObjectName("chatEyebrow");
    eyebrow->setAlignment(Qt::AlignCenter);
    welcome->addWidget(eyebrow);
    m_emptyConversation = new QLabel("Your model. Your way.", m_welcome);
    m_emptyConversation->setObjectName("emptyConversation");
    m_emptyConversation->setAlignment(Qt::AlignCenter);
    m_emptyConversation->setWordWrap(true);
    welcome->addWidget(m_emptyConversation);
    auto *hint = new QLabel("Choose the model. Bring your knowledge. Make it yours.", m_welcome);
    hint->setObjectName("cardMeta");
    hint->setAlignment(Qt::AlignCenter);
    hint->setWordWrap(true);
    welcome->addWidget(hint);
    auto *suggestions = new QHBoxLayout();
    struct WorkspaceAction {
        QString label;
        QString description;
    };
    const WorkspaceAction workspaceActions[] = {
        {"Try a prompt", "Draft a reasoning prompt for your selected model."},
        {"Ask docs", "Enable Knowledge and draft a question about your ingested documents."},
        {"Tune response", "Open engine, response length and temperature settings."}
    };
    for (int i = 0; i < 3; ++i) {
        auto *button = new Button(workspaceActions[i].label, m_welcome);
        button->setObjectName("promptSuggestion");
        button->setProperty("wordWrap", true);
        QSizePolicy cardPolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        cardPolicy.setHeightForWidth(true);   // the card wraps its text, so its height depends on its width
        button->setSizePolicy(cardPolicy);
        button->setToolTip(workspaceActions[i].description);
        suggestions->addWidget(button, 1);
        connect(button, &QPushButton::clicked, this, [this, i]() {
            if (i == 2) {
                m_settingsToggle->setChecked(true);
                m_tempSpin->setFocus();
                return;
            }
            if (i == 1) {
                m_ragCheck->setChecked(true);
                showKnowledge();
            }
            m_input->setPlainText(i == 0
                ? "Walk me through your reasoning on this problem: "
                : "Using my documents, help me understand: ");
            auto cursor = m_input->textCursor();
            cursor.movePosition(QTextCursor::End);
            m_input->setTextCursor(cursor);
            m_input->setFocus();
        });
    }
    welcome->addLayout(suggestions);
    welcome->addStretch();
    m_chatLayout->addWidget(m_welcome, 1);
    m_chatScroll->setWidget(m_chatHost);
    conversation->addWidget(m_chatScroll, 1);
    EdgeScrollBar::attach(m_chatScroll);

    auto *composer = Surface::card(this);
    composer->parentWidget()->setObjectName("chatComposer");
    composer->setContentsMargins(16, 12, 12, 10);
    m_input = new QPlainTextEdit(this);
    m_input->setObjectName("chatInput");
    m_input->setPlaceholderText("Start a conversation with your model…");
    m_input->setAccessibleName("Chat message");
    m_input->setFixedHeight(64);
    m_input->setTabChangesFocus(true);
    m_input->installEventFilter(this);
    composer->addWidget(m_input);
    auto *actions = new QHBoxLayout();
    m_previewMode = new ComboBox(this);
    m_previewMode->setAccessibleName("Chat mode");
    m_previewMode->addItems({"Chat", "Blueprint", "Tool mode"});
    m_previewMode->hide();
    actions->addWidget(m_previewMode);
    m_ragCheck = new ToggleSwitch("Your knowledge", this);
    m_ragCheck->setToolTip("Include matching passages from documents in your Knowledge workspace.");
    actions->addWidget(m_ragCheck);
    m_knowledgeContext = new Button("Knowledge off", this);
    m_knowledgeContext->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    m_knowledgeContext->setMinimumWidth(145);
    m_knowledgeContext->setProperty("themeIcon", "database");
    m_knowledgeContext->setIcon(Theme::coloredIcon("database", QColor(Theme::textSecondary()), 16));
    m_knowledgeContext->hide();
    actions->addWidget(m_knowledgeContext);
    connect(m_knowledgeContext, &QPushButton::clicked, this, &ChatPanel::showKnowledge);
    connect(m_ragCheck, &QCheckBox::toggled, this, &ChatPanel::updateKnowledgeContext);
    m_activity = new StudioCanvas(this);
    m_activity->setProperty("brandMark",true);
    m_activity->setProperty("activityIndicator",true);
    m_activity->setObjectName("chatActivity");
    m_activity->setFixedSize(32,32);
    m_activity->hide();
    actions->addWidget(m_activity);
    actions->addStretch();
    m_meter = new ContextMeter(this);
    actions->addWidget(m_meter->ring());
    m_stopButton = new Button("Stop", this);
    m_stopButton->setEnabled(false);
    actions->addWidget(m_stopButton);
    m_sendButton = new Button("Send  ↑", this);
    m_sendButton->setObjectName("primaryBtn");
    actions->addWidget(m_sendButton);
    composer->addLayout(actions);
    conversation->addWidget(m_meter->banner());
    conversation->addWidget(composer->parentWidget());
    conversation->addWidget(m_meter->bar());
    auto *shortcutHint = new QLabel("Enter to send · Shift+Enter for a new line", this);
    shortcutHint->setProperty("chatShortcutHint",true);
    shortcutHint->setAlignment(Qt::AlignCenter);
    shortcutHint->setObjectName("cardMeta");
    conversation->addWidget(shortcutHint);
    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName("statusLabel");
    m_statusLabel->setTextFormat(Qt::PlainText);
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setAlignment(Qt::AlignCenter);
    conversation->addWidget(m_statusLabel);
    connect(m_sendButton, &QPushButton::clicked, this, &ChatPanel::onSend);
    connect(m_stopButton, &QPushButton::clicked, this, &ChatPanel::onStop);
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(150);
    connect(m_saveTimer, &QTimer::timeout, this, &ChatPanel::flushSave);
    m_cursorTimer = new QTimer(this);
    m_cursorTimer->setInterval(500);
    connect(m_cursorTimer, &QTimer::timeout, this, [this]() {
        if (QSettings().value("appearance/studio3ReducedMotion",false).toBool()) {
            m_cursorTimer->stop();
            m_cursorVisible = false;
            if (m_streamBubble) m_streamBubble->setCursorVisible(false);
            return;
        }
        m_cursorVisible = !m_cursorVisible;
        if (m_streamBubble) m_streamBubble->setCursorVisible(m_cursorVisible);
    });
    connect(m_input,&QPlainTextEdit::textChanged,this,&ChatPanel::updateActions);
    m_usageTimer = new QTimer(this);
    m_usageTimer->setSingleShot(true);
    m_usageTimer->setInterval(400);
    connect(m_usageTimer, &QTimer::timeout, this, &ChatPanel::refreshContextUsage);
    auto usageMayHaveChanged = [this]() { if (m_modelLoaded) m_usageTimer->start(); };
    connect(m_input, &QPlainTextEdit::textChanged, this, usageMayHaveChanged);
    connect(m_maxTokensSpin, qOverload<int>(&QSpinBox::valueChanged), this, usageMayHaveChanged);
    connect(m_meter, &ContextMeter::newChatRequested, this, &ChatPanel::onClearChat);
    reloadContextSettings();
    updateActions();
    setStatus("Choose and load a model to get started.");
}

bool ChatPanel::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_input && (event->type() == QEvent::FocusIn || event->type() == QEvent::FocusOut)) {
        auto *composer = m_input->parentWidget();
        composer->setProperty("focused",event->type() == QEvent::FocusIn);
        composer->style()->unpolish(composer);
        composer->style()->polish(composer);
        composer->update();
    }
    if (watched == m_input && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
            && !(key->modifiers() & Qt::ShiftModifier)
            && (QSettings().value("chat/sendKey","enter").toString() != "modified"
                || (key->modifiers() & Qt::ControlModifier))) {
            if (!key->isAutoRepeat() && m_sendButton->isEnabled()) onSend();
            return true;
        }
    }
    return BasePanel::eventFilter(watched, event);
}

void ChatPanel::setBackendClient(BackendClient *client) {
    m_client = client;
    refreshUtilization();
    applyEngineStatus({});
    setStatus(client && client->isReady()
                  ? "Backend connected. Loading model list..."
                  : "Backend not connected.");
    refreshModels();
    refreshEngineStatus();
}

void ChatPanel::refreshModels() {
    if (!m_client || !m_client->isReady()) return;
    m_client->get("/models", [panel = QPointer<ChatPanel>(this)](int status, const QJsonDocument &doc) {
        if (!panel || status != 200) return;
        auto *picker = panel->m_modelCombo;
        const QJsonArray models = doc.array();
        const QSignalBlocker blockModelSignals(picker);
        const QString prev = picker->currentText();
        picker->clear();
        for (const QJsonValue &v : models) {
            const QJsonObject m = v.toObject();
            const QString id = m["id"].toString();
            if (id.isEmpty()) continue;
            picker->addItem(id);
            picker->setItemData(picker->count() - 1, id, Qt::ToolTipRole);
        }
        // Restore previous text if still present
        const int idx = picker->findText(prev);
        if (idx >= 0) picker->setCurrentIndex(idx);
        else picker->setEditText(prev);
    });
}

void ChatPanel::refreshEngineStatus() {
    if (!m_client || !m_client->isReady()) applyUtilization({});
    requestEngineStatus();
}

void ChatPanel::showEvent(QShowEvent *event) {
    BasePanel::showEvent(event);
    refreshUtilization();
}

void ChatPanel::refreshUtilization() {
    if (!isVisible() || m_utilizationPending) return;
    if (!m_client || !m_client->isReady()) {
        applyUtilization({});
        return;
    }
    m_utilizationPending = true;
    m_client->get("/training/telemetry", [panel = QPointer<ChatPanel>(this)](int status, const QJsonDocument &doc) {
        if (!panel) return;
        panel->m_utilizationPending = false;
        panel->applyUtilization(status == 200 ? doc.object() : QJsonObject{});
    });
}

void ChatPanel::applyUtilization(const QJsonObject &data) {
    m_utilization->applyTelemetry(data);
    const QString title = m_utilization->label();
    const QString reading = m_utilization->reading();
    m_utilizationLabel->setText(title.section(' ', 0, 0) + " " + reading);
    const QString description = title + (reading == "—" ? " unavailable" : " " + reading);
    m_utilizationLabel->setAccessibleName(description);
    m_utilizationLabel->setToolTip(description);
    m_utilization->setToolTip(description);
}

void ChatPanel::requestEngineStatus(const QString &error) {
    if (m_modelOperation != ModelOperation::Idle) return;
    const quint64 request = ++m_engineStatusRequest;
    if (!m_client || !m_client->isReady()) {
        applyEngineStatus({});
        setStatus("Backend not connected.");
        return;
    }
    m_client->get("/inference/status", [panel = QPointer<ChatPanel>(this), request, error](int status, const QJsonDocument &doc) {
        if (!panel || request != panel->m_engineStatusRequest) return;
        panel->applyEngineStatus(status == 200 ? doc.object() : QJsonObject{});
        if (!error.isEmpty()) panel->setStatus(error, "error");
        else if (status != 200) panel->setStatus("Inference status unavailable.");
        else if (panel->m_modelLoaded) panel->setStatus("Engine ready.");
        else panel->setStatus("No inference engine loaded. Load a model to begin.");
    });
}

void ChatPanel::applyEngineStatus(const QJsonObject &status) {
    const auto engine = status["engine"].toObject();
    m_modelLoaded = !engine.isEmpty();
    m_enginePill->setText("engine: " + (m_modelLoaded ? engine["description"].toString() : QStringLiteral("none")));
    m_modelPill->setText("model: " + (m_modelLoaded ? status["loaded_model"].toString() : QStringLiteral("—")));
    const QString adapter = status["active_adapter"].toString();
    m_adapterPill->setText("adapter: " + adapter);
    m_adapterPill->setVisible(m_modelLoaded && !adapter.isEmpty());
    refreshContextUsage();
    updateActions();
}

QString ChatPanel::currentEngineName() const {
    switch (m_engineCombo->currentIndex()) {
        case 1: return "vllm";
        case 2: return "llamacpp";
        case 3: return "torch";
        case 4: return "ollama";
        default: return QString();
    }
}

void ChatPanel::onLoadModel() {
    if (isBusy()) return;
    if (!m_client || !m_client->isReady()) {
        setStatus("Backend not connected.", "error");
        return;
    }
    const QString modelRef = m_modelCombo->currentText().trimmed();
    if (modelRef.isEmpty() || modelRef.startsWith("(")) {
        setStatus("Enter or select a model id.", "error");
        return;
    }
    const QString engineName = currentEngineName();
    QJsonObject body{{"model", modelRef}};
    if (!engineName.isEmpty()) body["engine"] = engineName;

    ++m_engineStatusRequest;
    m_modelOperation = ModelOperation::Loading;
    updateActions();
    setStatus("Loading model… First use may download model files.");

    m_client->post("/inference/load", body, [panel = QPointer<ChatPanel>(this)](int status, const QJsonDocument &doc) {
        if (!panel) return;
        panel->m_modelOperation = ModelOperation::Idle;
        if (status == 200) {
            const auto model = doc.object();
            panel->applyEngineStatus(model);
            panel->setStatus("Loaded " + model["loaded_model"].toString() + ".");
            panel->m_input->setFocus();
        } else {
            panel->updateActions();
            const auto error = "Load failed: " + doc.object()["detail"].toString(QString("HTTP %1").arg(status));
            panel->setStatus(error, "error");
            panel->requestEngineStatus(error);
        }
    });
}

void ChatPanel::onUnloadModel() {
    if (isBusy() || !m_modelLoaded || !m_client || !m_client->isReady()) return;

    ++m_engineStatusRequest;
    m_modelOperation = ModelOperation::Unloading;
    updateActions();
    setStatus("Unloading model…");

    m_client->post("/inference/unload", {}, [panel = QPointer<ChatPanel>(this)](int status, const QJsonDocument &doc) {
        if (!panel) return;
        panel->m_modelOperation = ModelOperation::Idle;
        if (status == 200) {
            panel->applyEngineStatus({});
            panel->setStatus("Model unloaded. Load a model to continue.");
        } else {
            panel->updateActions();
            const auto error = "Unload failed: " + doc.object()["detail"].toString(QString("HTTP %1").arg(status));
            panel->setStatus(error, "error");
            panel->requestEngineStatus(error);
        }
    });
}

void ChatPanel::ensureTrailingStretch() {
    for (int i = 0; i < m_chatLayout->count(); ++i) {
        if (m_chatLayout->itemAt(i)->spacerItem()) {
            delete m_chatLayout->takeAt(i);
            --i;
        }
    }
    m_chatLayout->addStretch(1);
}

void ChatPanel::onSend() {
    if (m_modelOperation != ModelOperation::Idle || m_preparing) return;
    const QString text = m_input->toPlainText().trimmed();
    if (text.isEmpty() || m_streaming) return;
    if (anyEditing()) {
        setStatus("Finish or cancel your edit first.", "warn");
        return;
    }
    beginSend(PendingSend{PendingSend::NewMessage, ChatTree::kRoot, text});
}

bool ChatPanel::beginSend(const PendingSend &send) {
    if (m_modelOperation != ModelOperation::Idle || m_preparing || m_streaming) return false;
    if (!m_client || !m_client->isReady()) {
        setStatus("Backend not connected.", "error");
        return false;
    }
    if (!m_modelLoaded) {
        setStatus("Load a model to continue.", "warn");
        return false;
    }
    m_activeSend = send;
    // Only a conversation longer than what compaction keeps verbatim can be compacted.
    const int prefix = prefixCount(send);
    const int uncompacted = prefix - m_tree.compaction(prefix).covered + (send.kind == PendingSend::Retry ? 0 : 1);
    if (m_autoCompactPercent > 0 && m_modelLoaded && uncompacted > kKeepRecent)
        measureThenSend(send);
    else
        startSend(send);
    return true;
}

MessageRow *ChatPanel::makeRow(ChatBubble::Role role, ChatTree::NodeId node, const QString &text) {
    auto *row = new MessageRow(role, node, text, m_chatHost);
    connect(row, &MessageRow::versionStep, this, &ChatPanel::onVersionStep);
    connect(row, &MessageRow::retryClicked, this, &ChatPanel::onRetry);
    connect(row, &MessageRow::editRequested, this, &ChatPanel::onEditRequested);
    connect(row, &MessageRow::editSaved, this, &ChatPanel::onEditSaved);
    connect(row, &MessageRow::editCancelled, this, [this]() { updateActions(); });
    addCitations(row, m_tree.citations(node));
    return row;
}

MessageRow *ChatPanel::rowForNode(ChatTree::NodeId node) const {
    if (!m_chatHost) return nullptr;
    for (MessageRow *row : m_chatHost->findChildren<MessageRow *>())
        if (row->node() == node) return row;
    return nullptr;
}

bool ChatPanel::anyEditing() const {
    if (!m_chatHost) return false;
    for (MessageRow *row : m_chatHost->findChildren<MessageRow *>())
        if (row->isEditing()) return true;
    return false;
}

void ChatPanel::refreshRows() {
    if (!m_chatHost) return;
    const bool busy = m_streaming || m_preparing || m_modelOperation != ModelOperation::Idle;
    const bool editing = anyEditing();
    const ChatTree::NodeId last = m_tree.lastOnPath();
    const bool canRetry = !busy && !editing && last != ChatTree::kRoot && m_tree.role(last) == "assistant";
    for (MessageRow *row : m_chatHost->findChildren<MessageRow *>()) {
        row->setActionsEnabled(!busy);
        row->setSwitcherEnabled(!busy && !editing);   // a redraw would destroy an open editor
        row->setRetryAvailable(canRetry && row->node() == last);
        row->setResendable(row->node() == last && m_tree.role(last) == "user");
        if (row->node() != ChatTree::kRoot) row->setVersion(m_tree.versionIndex(row->node()), m_tree.versionCount(row->node()));
    }
}

void ChatPanel::onVersionStep(ChatTree::NodeId node, int delta) {
    if (m_streaming || m_preparing || m_modelOperation != ModelOperation::Idle || anyEditing()) return;
    const QList<ChatTree::NodeId> versions = m_tree.versionsOf(node);
    const int target = m_tree.versionIndex(node) + delta;
    if (target < 0 || target >= versions.size()) return;
    m_tree.activate(versions.at(target));
    markDirty();
    // Keep the reader where they were rather than jumping to the bottom.
    QScrollBar *bar = m_chatScroll->verticalScrollBar();
    const int position = bar->value();
    renderTranscript();
    QTimer::singleShot(0, this, [bar, position]() { bar->setValue(qMin(position, bar->maximum())); });
    refreshContextUsage();
}

void ChatPanel::onEditRequested(ChatTree::NodeId node) {
    if (m_streaming || m_preparing || m_modelOperation != ModelOperation::Idle) return;
    for (MessageRow *row : m_chatHost->findChildren<MessageRow *>())
        if (row->isEditing() && row->node() != node) row->cancelEdit();
    if (MessageRow *row = rowForNode(node)) row->beginEdit(row->bubble()->text());
    updateActions();
}

void ChatPanel::onEditSaved(ChatTree::NodeId node, const QString &text) {
    // The editor has already closed. If the send cannot start, give the text back.
    // Saving a message that never got a reply, unchanged, sends it again rather than adding a second copy.
    const bool resend = node == m_tree.lastOnPath() && m_tree.role(node) == "user" && text == m_tree.content(node);
    if (!beginSend(PendingSend{resend ? PendingSend::Retry : PendingSend::Edit, node, text})) {
        if (MessageRow *row = rowForNode(node)) row->beginEdit(text);
    }
    updateActions();
}

int ChatPanel::prefixCount(const PendingSend &send) const {
    if (send.kind == PendingSend::NewMessage) return m_tree.activePath().size();
    // A retry anchored on a user message (one that never got a reply) keeps that message in the prefix.
    return m_tree.pathIndexOf(send.anchor) + (send.kind == PendingSend::Retry && m_tree.role(send.anchor) == "user" ? 1 : 0);
}

bool ChatPanel::commitReply() {
    const bool retry = m_streamKind == PendingSend::Retry;
    const QStringList notes = m_pendingNotes;   // a retry's compaction dividers wait here until its reply exists
    m_pendingNotes.clear();
    m_pathLimit = -1;
    const std::optional<ChatTree::Compaction> retryCompaction = m_retryCompaction;
    m_retryCompaction.reset();
    const QPointer<MessageRow> row = m_streamRow;
    m_streamRow = nullptr;
    if (m_pendingResponse.isEmpty()) {
        if (retry) renderTranscript();   // nothing came back: put the previous reply back on screen
        return false;
    }
    const ChatTree::NodeId reply = m_tree.addChild(m_replyParent, "assistant", m_pendingResponse);
    if (retry) {
        for (const QString &note : notes) m_tree.addNote(reply, note);
        if (retryCompaction) m_tree.setCompaction(reply, *retryCompaction);
    }
    m_tree.setCitations(reply, m_pendingSources);
    if (row) {
        row->setNode(reply);
        addCitations(row, m_pendingSources);
    }
    markDirty();
    return true;
}

void ChatPanel::onRetry(ChatTree::NodeId reply) {
    if (m_streaming || m_preparing || m_modelOperation != ModelOperation::Idle || anyEditing()) return;
    if (reply == ChatTree::kRoot || reply != m_tree.lastOnPath() || m_tree.role(reply) != "assistant") return;
    beginSend(PendingSend{PendingSend::Retry, reply, m_tree.content(m_tree.parentOf(reply))});
}

void ChatPanel::measureThenSend(const PendingSend &send) {
    m_preparing = true;
    updateActions();
    m_stopButton->setEnabled(true);
    QJsonArray messages = effectiveMessages(prefixCount(send));
    if (send.kind != PendingSend::Retry) messages.append(QJsonObject{{"role", "user"}, {"content", send.text}});
    QJsonObject body;
    body["messages"] = messages;
    body["max_tokens"] = m_maxTokensSpin->value();
    const quint64 requestId = ++m_prepareRequest;
    m_prepareReply = m_client->post("/inference/context-usage", body, [this, requestId, send](int status, const QJsonDocument &doc) {
        if (requestId != m_prepareRequest) return;
        bool compact = false;
        if (status == 200) {
            const QJsonObject usage = doc.object();
            const int limit = usage["limit"].toInt();
            // Compaction cannot help when the reply reserve alone is already at the threshold.
            compact = limit > 0 &&
                      ContextMeterLogic::shouldCompact(usage["fill"].toDouble(), m_autoCompactPercent) &&
                      double(usage["reserved"].toInt()) / limit < m_autoCompactPercent / 100.0;
        }
        if (compact) {
            compactThenSend(send);
        } else {
            m_preparing = false;
            m_stopButton->setEnabled(false);
            updateActions();
            startSend(send);
        }
    });
}

void ChatPanel::compactThenSend(const PendingSend &send) {
    setStatus("Compacting…");
    const int prefix = prefixCount(send);
    const ChatTree::Compaction base = m_tree.compaction(prefix);
    const QList<QJsonObject> path = m_tree.messages(prefix);
    QJsonArray posted;
    for (int i = base.covered; i < path.size(); ++i) posted.append(path.at(i));
    if (send.kind != PendingSend::Retry) posted.append(QJsonObject{{"role", "user"}, {"content", send.text}});
    QJsonObject body;
    body["messages"] = posted;
    body["previous_summary"] = base.summary;
    body["max_tokens"] = m_maxTokensSpin->value();
    body["target_fill"] = m_autoCompactPercent / 100.0;
    body["keep_recent"] = kKeepRecent;
    const quint64 requestId = m_prepareRequest;
    m_prepareReply = m_client->post("/inference/compact", body, [this, requestId, send, base](int status, const QJsonDocument &doc) {
        if (requestId != m_prepareRequest) return;
        m_preparing = false;
        m_stopButton->setEnabled(false);
        updateActions();
        if (status == 200) {
            const QJsonObject result = doc.object();
            const int covered = result["covered"].toInt();
            if (covered > 0) {
                const bool summarized = result["method"].toString() == "summary";
                ChatTree::Compaction next = base;
                next.covered += covered;
                if (summarized) next.summary = result["summary"].toString();
                m_pendingCompaction = next;
                const QString note = summarized ? "Context compacted — earlier messages were summarized"
                                                : "Context compacted — earlier messages were dropped to make room";
                m_pendingNotes.append(note);
            }
        } else {
            m_pendingNotes.append("Could not compact earlier messages; sent as is.");
        }
        startSend(send);
    });
}

void ChatPanel::cancelPreparing() {
    ++m_prepareRequest;
    // Abort only the request being awaited (the backend cancels a compaction when its
    // connection closes); other panels' requests share this client and must go on.
    if (m_prepareReply) m_prepareReply->abort();
    m_preparing = false;
    m_stopButton->setEnabled(false);
    if (m_activeSend.kind == PendingSend::Edit) {   // give the typed edit back
        if (MessageRow *row = rowForNode(m_activeSend.anchor)) row->beginEdit(m_activeSend.text);
    }
    m_activeSend = PendingSend();
    updateActions();
    setStatus("Compaction stopped.");
}

void ChatPanel::addContextDivider(const QString &text) {
    auto *divider = new QLabel(text, m_chatHost);
    divider->setObjectName("contextDivider");
    divider->setAlignment(Qt::AlignCenter);
    divider->setWordWrap(true);
    m_chatLayout->addWidget(divider);
    ensureTrailingStretch();
}

void ChatPanel::renderTranscript(int pathCount, const QStringList &trailingNotes) {
    // Remove everything but the welcome panel. Detach first so findChildren() never sees stale widgets.
    while (QLayoutItem *item = m_chatLayout->takeAt(0)) {
        if (QWidget *w = item->widget(); w && w != m_welcome) {
            w->hide();
            w->setParent(nullptr);
            w->deleteLater();
        }
        delete item;
    }
    if (pathCount < 0) pathCount = m_pathLimit;
    const QList<ChatTree::NodeId> path = m_tree.activePath();
    const int count = pathCount < 0 ? path.size() : qMin(pathCount, int(path.size()));
    if (count == 0 && trailingNotes.isEmpty()) {
        m_welcome->show();
        m_chatLayout->addWidget(m_welcome, 1);
    } else {
        m_welcome->hide();
    }
    for (int i = 0; i < count; ++i) {
        const ChatTree::NodeId id = path.at(i);
        for (const QString &note : m_tree.notes(id)) addContextDivider(note);
        const auto role = m_tree.role(id) == "user" ? ChatBubble::User : ChatBubble::Assistant;
        m_chatLayout->addWidget(makeRow(role, id, m_tree.content(id)));
    }
    for (const QString &note : trailingNotes) addContextDivider(note);
    ensureTrailingStretch();
    refreshRows();
}

void ChatPanel::redrawForTest() { renderTranscript(); }

void ChatPanel::startSend(const PendingSend &send) {
    const QString text = send.text;
    m_activeSend = PendingSend();
    m_followOutput = true;
    if (send.kind == PendingSend::NewMessage) m_input->clear();   // an edit leaves the composer's draft alone

    // Add the user message to the conversation (an edit becomes a new version of the message it
    // replaces), with the compaction and dividers that led to it.
    m_streamKind = send.kind;
    if (send.kind == PendingSend::Retry) {
        // No new user message: the reply is made for the existing one. Only the messages up to it
        // count until the new reply exists, and the old reply is not drawn while it streams.
        const bool onUserMessage = m_tree.role(send.anchor) == "user";
        m_streamHadReply = !onUserMessage;
        m_replyParent = onUserMessage ? send.anchor : m_tree.parentOf(send.anchor);
        m_pathLimit = prefixCount(send);
        m_retryCompaction = m_pendingCompaction;   // stored on the new reply once it has text
        m_pendingCompaction.reset();
        renderTranscript(m_pathLimit, m_pendingNotes);   // the dividers join the tree with the new reply
    } else {
        const ChatTree::NodeId parent = send.kind == PendingSend::Edit ? m_tree.parentOf(send.anchor) : m_tree.lastOnPath();
        m_replyParent = m_tree.addChild(parent, "user", text);
        if (m_pendingCompaction) m_tree.setCompaction(m_replyParent, *m_pendingCompaction);
        for (const QString &note : m_pendingNotes) m_tree.addNote(m_replyParent, note);
        m_pendingCompaction.reset();
        m_pendingNotes.clear();
        markDirty();
        // Draw the conversation, then an empty reply row that is streamed into below.
        renderTranscript();
    }
    m_streamRow = makeRow(ChatBubble::Assistant, ChatTree::kRoot, QString());
    m_chatLayout->addWidget(m_streamRow);
    m_streamBubble = m_streamRow->bubble();
    ensureTrailingStretch();
    scrollToBottom();

    m_pendingResponse.clear();
    m_pendingSources = {};
    m_streamFailed = false;
    const quint64 requestId = ++m_requestId;
    m_streaming = true;
    updateActions();
    m_cursorVisible = false;
    m_activity->setActivity(StudioCanvas::Chat);
    m_activity->setMotionEnabled(!QSettings().value("appearance/studio3ReducedMotion",false).toBool());
    m_activity->show();
    m_sendButton->setText("Responding…");
    if (!QSettings().value("appearance/studio3ReducedMotion",false).toBool()) m_cursorTimer->start();
    m_sendButton->setEnabled(false);
    m_stopButton->setEnabled(true);

    QJsonObject body;
    body["messages"] = effectiveMessages();
    body["max_tokens"] = m_maxTokensSpin->value();
    body["temperature"] = m_tempSpin->value();
    body["stream"] = true;
    QJsonObject knowledge{{"enabled", m_ragCheck->isChecked()}};
    if (m_knowledge && m_knowledge->documentScope().isArray())
        knowledge["document_ids"] = m_knowledge->documentScope();
    body["rag"] = knowledge;

    setStatus("Generating...");
    m_client->streamPost(
        "/inference/chat",
        body,
        // onEvent
        [this, requestId](const QJsonObject &event) {
            if (requestId != m_requestId) return;
            if (event.contains("error")) {
                m_streamFailed = true;
                setStatus("Generation error: " + event.value("error").toString(), "error");
                updateStreamingBubble(m_pendingResponse, false);
                return;
            }
            if (event["sources"].isArray()) m_pendingSources = event["sources"].toArray();
            const QString delta = event.value("delta").toString();
            if (!delta.isEmpty()) {
                m_pendingResponse += delta;
                updateStreamingBubble(m_pendingResponse, true);
            }
            if (event.value("done").toBool()) {
                updateStreamingBubble(m_pendingResponse, false);
                const bool committed = commitReply();
                const bool retryFailed = !committed && m_streamKind == PendingSend::Retry;
                m_streamBubble = nullptr;
                m_streaming = false;
                m_activity->hide();
                m_sendButton->setText("Send  ↑");
                m_cursorTimer->stop();
                m_cursorVisible = false;
                updateActions();
                m_stopButton->setEnabled(false);
                if (retryFailed) setStatus(m_streamHadReply ? "Retry failed; kept the previous reply." : "No reply received.", "warn");
                else setStatus("Ready.");
                refreshContextUsage();
            }
        },
        // onDone
        [this, requestId](int statusCode) {
            if (requestId != m_requestId) return;
            if (!m_streamFailed && statusCode != 200) {
                const QString err = m_pendingResponse.isEmpty()
                                       ? QString("HTTP %1").arg(statusCode)
                                       : m_pendingResponse;
                setStatus("Generation error: " + err, "error");
            } else if (!m_streamFailed && m_streaming) {
                setStatus("Generation ended unexpectedly. Check the backend and retry.", "error");
            }
            // If not already finalized by done event
            if (m_streaming) {
                updateStreamingBubble(m_pendingResponse, false);
                commitReply();
                m_streamBubble = nullptr;
                m_streaming = false;
                m_activity->hide();
                m_sendButton->setText("Send  ↑");
                m_cursorTimer->stop();
                m_cursorVisible = false;
                updateActions();
                m_stopButton->setEnabled(false);
                refreshContextUsage();
            }
        });
}

void ChatPanel::onStop() {
    if (m_preparing) {
        cancelPreparing();
        return;
    }
    if (!m_streaming || !m_client) return;
    ++m_requestId;
    m_client->abortStream();
    updateStreamingBubble(m_pendingResponse, false);
    commitReply();   // keeps any text so far; for a retry with none, puts the previous reply back
    m_streamBubble = nullptr;
    m_streaming = false;
    m_activity->hide();
    m_sendButton->setText("Send  ↑");
    m_cursorTimer->stop();
    m_cursorVisible = false;
    updateActions();
    m_stopButton->setEnabled(false);
    setStatus("Generation stopped.");
    refreshContextUsage();
}

void ChatPanel::stopActivity() {
    if (m_preparing) cancelPreparing();
    if (m_streaming) onStop();
}

void ChatPanel::onClearChat() {
    showOrdinaryChat();
    // "New chat": the chat on screen stays saved, a blank one takes its place.
    stopActivity();
    flushSave();
    m_chatId.clear();
    resetConversation();
    restoreKnowledge();
    setStatus("New conversation.");
    refreshContextUsage();
    emit currentChatChanged(QString());
}

void ChatPanel::resetConversation(bool showBlank) {
    m_tree.clear();
    m_replyParent = ChatTree::kRoot;
    m_pathLimit = -1;
    m_retryCompaction.reset();
    m_streamKind = PendingSend::NewMessage;
    m_streamRow = nullptr;
    m_pendingCompaction.reset();
    m_pendingNotes.clear();
    m_activeSend = PendingSend();
    // Tear down streaming state.
    m_streaming = false;
    m_activity->hide();
    m_sendButton->setText("Send  ↑");
    m_cursorTimer->stop();
    m_cursorVisible = false;
    m_streamBubble = nullptr;
    m_pendingResponse.clear();
    m_pendingSources = {};
    // Remove every bubble and show the welcome panel.
    while (QLayoutItem *item = m_chatLayout->takeAt(0)) {
        if (QWidget *w = item->widget(); w && w != m_welcome) {
            w->hide();
            w->setParent(nullptr);
            w->deleteLater();
        }
        delete item;
    }
    m_welcome->show();
    m_chatLayout->addWidget(m_welcome, 1);
    m_chatLayout->invalidate();
    m_chatHost->updateGeometry();
    if (showBlank) showBlankChat();   // opening a saved chat does not use up a greeting nobody sees
    updateActions();
    m_stopButton->setEnabled(false);
}

QVBoxLayout *ChatPanel::addDrawer(const QString &title, const QString &name, QPushButton *toggle) {
    auto *drawer = new QFrame(this);
    drawer->setObjectName(name);
    drawer->setProperty("conversationDrawer", true);
    drawer->setFixedWidth(316);
    auto *body = new QVBoxLayout(drawer);
    body->setContentsMargins(14, 12, 14, 12);
    auto *header = new QHBoxLayout;
    auto *label = new QLabel(title, drawer);
    label->setObjectName("hubSectionTitle");
    header->addWidget(label, 1);
    auto *close = toolbarIcon(drawer, "Close " + title, "window-close");
    header->addWidget(close);
    body->addLayout(header);
    m_workspace->addWidget(drawer);
    drawer->hide();
    connect(toggle, &QPushButton::toggled, drawer, &QWidget::setVisible);
    connect(close, &QPushButton::clicked, this, [toggle] {
        toggle->setChecked(false);
        toggle->setFocus();
    });
    return body;
}

void ChatPanel::setKnowledgePanel(RagPanel *panel) {
    m_knowledge = panel;
    auto *body = addDrawer("Knowledge", "conversationKnowledge", m_knowledgeToggle);
    m_knowledgeDrawer = body->parentWidget();
    m_ragCheck->setText("Use in this conversation");
    body->addWidget(m_ragCheck);
    auto *scroll = new QScrollArea(body->parentWidget());
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->setWidget(panel);
    EdgeScrollBar::attachInside(scroll);   // the bar floats over the edge and takes no width from the drawer
    panel->setAutoFillBackground(false);
    body->addWidget(scroll, 1);
    m_knowledgeToggle->show();
    m_knowledgeContext->show();
    connect(panel, &RagPanel::sourceScopeChanged, this, &ChatPanel::updateKnowledgeContext);
    connect(m_knowledgeToggle, &QPushButton::toggled, this, [this](bool open) { if (open) fitKnowledgeDrawer(); });
    updateKnowledgeContext();
    fitKnowledgeDrawer();
}

void ChatPanel::fitKnowledgeDrawer() {
    if (!m_knowledgeDrawer || !m_knowledge) return;
    constexpr int kDrawerWidth = 316;   // the designed width at the default interface size
    const QMargins margins = m_knowledgeDrawer->layout()->contentsMargins();
    // The panel scrolls its own content, so its minimum hides what that content needs: ask the content.
    int panelNeeds = m_knowledge->minimumSizeHint().width();
    if (auto *inner = m_knowledge->findChild<QScrollArea *>("cardScroll"); inner && inner->widget()) {
        const QMargins panelMargins = m_knowledge->layout() ? m_knowledge->layout()->contentsMargins() : QMargins();
        panelNeeds = qMax(panelNeeds, inner->widget()->minimumSizeHint().width() + panelMargins.left() + panelMargins.right());
    }
    const int needed = panelNeeds + margins.left() + margins.right();
    m_knowledgeDrawer->setFixedWidth(qMax(kDrawerWidth, needed));
}

void ChatPanel::changeEvent(QEvent *event) {
    BasePanel::changeEvent(event);
    // A new interface size changes how much room the drawer's buttons need; measure once the new style has settled.
    if ((event->type() == QEvent::StyleChange || event->type() == QEvent::FontChange) && m_knowledgeDrawer && !m_drawerFitPending) {
        m_drawerFitPending = true;
        QTimer::singleShot(0, this, [this] { m_drawerFitPending = false; fitKnowledgeDrawer(); });
    }
}

void ChatPanel::addCitations(MessageRow *row, const QJsonArray &sources) {
    if (sources.isEmpty()) return;
    auto *citations = new FlowLayout;
    citations->setContentsMargins(0, 0, 0, 0);
    qobject_cast<QVBoxLayout *>(row->layout())->addLayout(citations);
    for (const auto &value : sources) {
        const auto source = value.toObject();
        const QString title = source["document_name"].toString();
        auto *chip = new Button(title, row);
        chip->setProperty("wordWrap", true);
        chip->setMaximumWidth(250);
        chip->setToolTip(title);
        citations->addWidget(chip);
        connect(chip, &QPushButton::clicked, this, [this, source, title] {
            Ui::LibraryDialog dialog(this, "Source passage", "Done");
            dialog.acceptButton()->hide();
            auto *heading = new QLabel(title, &dialog);
            heading->setTextFormat(Qt::PlainText);
            heading->setWordWrap(true);
            heading->setObjectName("hubSectionTitle");
            dialog.form()->addWidget(heading);
            auto *passage = new QPlainTextEdit(source["text"].toString(), &dialog);
            passage->setReadOnly(true);
            passage->setAccessibleName("Source passage");
            dialog.form()->addWidget(passage);
            dialog.resize(540, 300);
            dialog.exec();
        });
    }
}

void ChatPanel::updateKnowledgeContext() {
    const bool enabled = m_ragCheck->isChecked();
    const int count = m_knowledge ? m_knowledge->enabledSourceCount() : 0;
    m_knowledgeContext->setText(!enabled ? "Knowledge off" : count == 0 ? "Select sources" :
                                count == 1 ? "1 source enabled" : QString("%1 sources enabled").arg(count));
    if (!m_knowledge || m_restoringKnowledge) return;
    QJsonObject knowledge{{"enabled", enabled}};
    if (m_knowledge->documentScope().isArray()) knowledge["document_ids"] = m_knowledge->documentScope();
    if (m_tree.knowledge() == knowledge) return;
    m_tree.setKnowledge(knowledge);
    markDirty();
}

void ChatPanel::restoreKnowledge() {
    if (!m_knowledge) return;
    m_restoringKnowledge = true;
    const auto state = m_tree.knowledge();
    m_ragCheck->setChecked(state["enabled"].toBool());
    m_knowledge->setDocumentScope(state["document_ids"]);
    m_restoringKnowledge = false;
}

void ChatPanel::setBlueprintChat(BlueprintChatPanel *panel) {
    m_chatContents->addWidget(panel);
    m_previewMode->show();
    connect(m_previewMode, &QComboBox::activated, panel, [this, panel](int index) {
        if (index == 0) return;
        {
            const QSignalBlocker block(m_previewMode);
            m_previewMode->setCurrentIndex(0);
        }
        m_chatContents->setCurrentWidget(panel);
        panel->setMode(index);
    });
    connect(panel, &BlueprintChatPanel::ordinaryChatRequested, this, [this] {
        showOrdinaryChat();
        m_input->setFocus();
    });
}

void ChatPanel::showOrdinaryChat() {
    m_chatContents->setCurrentIndex(0);
}

void ChatPanel::showKnowledge() {
    showOrdinaryChat();
    if (m_knowledgeDrawer) m_knowledgeToggle->setChecked(true);
}

void ChatPanel::setChatStore(ChatStore *store) {
    if (m_store == store) return;
    flushSave();
    if (m_store) disconnect(m_store, nullptr, this, nullptr);
    m_store = store;
    // Whatever is on screen belongs to the previous store (or to none): it is not saved into the new one.
    m_saveDirty = false;
    m_saveTimer->stop();
    m_chatId.clear();
}

bool ChatPanel::isBusy() const { return m_streaming || m_preparing || m_modelOperation != ModelOperation::Idle || anyEditing(); }

void ChatPanel::markDirty() {
    if (!m_store || m_tree.isEmpty()) return;
    m_saveDirty = true;
    if (m_chatId.isEmpty()) {
        // The first change of a new chat is written at once, so a crash never loses the first message.
        m_chatId = m_store->create();
        flushSave();
        emit currentChatChanged(m_chatId);
        return;
    }
    m_saveTimer->start();
}

bool ChatPanel::flushSave() {
    if (!m_store || m_chatId.isEmpty() || !m_saveDirty) return true;
    m_saveTimer->stop();
    m_saveDirty = false;
    if (!m_store->save(m_chatId, m_tree)) {
        m_saveDirty = true;   // still unsaved: the next flush (switching chats, closing) tries again
        setStatus("Could not save this chat: " + m_store->lastError(), "warn");
        return false;
    }
    return true;
}

bool ChatPanel::openChat(const QString &id) {
    if (!m_store || isBusy()) return false;
    if (id == m_chatId) {
        showOrdinaryChat();
        return true;
    }
    const std::optional<ChatTree> loaded = m_store->load(id);
    if (!loaded) {
        setStatus("That chat could not be opened.", "error");
        return false;
    }
    showOrdinaryChat();
    flushSave();
    resetConversation(false);
    m_tree = *loaded;
    m_chatId = id;
    restoreKnowledge();
    renderTranscript();
    scrollToBottom();
    updateActions();
    setStatus("Ready.");
    refreshContextUsage();
    emit currentChatChanged(m_chatId);
    return true;
}

bool ChatPanel::renameChat(const QString &id, const QString &title) {
    if (!m_store || (id == m_chatId && isBusy())) return false;
    if (m_store->rename(id, title)) return true;
    setStatus("Could not rename that chat" + (m_store->lastError().isEmpty() ? QString(".") : ": " + m_store->lastError()), "warn");
    return false;
}

bool ChatPanel::pinChat(const QString &id, bool pinned) {
    if (!m_store || (id == m_chatId && isBusy())) return false;
    if (m_store->setPinned(id, pinned)) return true;
    setStatus(QString("Could not %1 that chat").arg(pinned ? "pin" : "unpin") + (m_store->lastError().isEmpty() ? QString(".") : ": " + m_store->lastError()), "warn");
    return false;
}

bool ChatPanel::deleteChat(const QString &id) {
    if (!m_store) return false;
    auto removeOrReport = [this](const QString &target) {
        if (m_store->remove(target)) return true;
        setStatus("Could not delete that chat" + (m_store->lastError().isEmpty() ? QString(".") : ": " + m_store->lastError()), "warn");
        return false;
    };
    if (id != m_chatId || m_chatId.isEmpty()) return removeOrReport(id);
    if (isBusy() || !removeOrReport(id)) return false;
    m_saveDirty = false;
    m_saveTimer->stop();
    m_chatId.clear();
    resetConversation();
    restoreKnowledge();
    setStatus("Conversation deleted.");
    refreshContextUsage();
    emit currentChatChanged(QString());
    return true;
}

QJsonArray ChatPanel::effectiveMessages(int pathCount) const {
    if (pathCount < 0) pathCount = m_pathLimit;
    const QList<QJsonObject> path = m_tree.messages(pathCount);
    const ChatTree::Compaction compaction = m_retryCompaction ? *m_retryCompaction : m_tree.compaction(pathCount);
    QJsonArray messages;
    if (compaction.covered > 0 && !compaction.summary.isEmpty())
        messages.append(QJsonObject{{"role", "system"},
                                    {"content", "Summary of the earlier conversation: " + compaction.summary}});
    for (int i = compaction.covered; i < path.size(); ++i) messages.append(path.at(i));
    return messages;
}

void ChatPanel::reloadContextSettings() {
    using namespace ContextMeterLogic;
    QSettings settings;
    for(auto* label:findChildren<QLabel*>()) if(label->property("chatShortcutHint").toBool()) {
        const auto send=settings.value("chat/sendKey","enter").toString()=="modified"?Shortcuts::modifiedSend():QKeySequence(Qt::Key_Enter);
        label->setText(Shortcuts::label(send)+" to send · "+Shortcuts::label(Shortcuts::newLine())+" for a new line");
    }
    m_meter->setStyle(parseStyle(settings.value("chat/meterStyle").toString()));
    m_autoCompactPercent = parseAutoCompact(settings.value("chat/autoCompactPercent"));
    m_meter->setAutoCompact(m_autoCompactPercent);
    applyMeterWarning();
    // A new name in Settings changes the greeting of a blank chat straight away.
    if (m_emptyConversation && m_tree.isEmpty() && Greeting::userName() != m_greetingName) showBlankChat();
}

void ChatPanel::refreshContextUsage() {
    if (!m_client || !m_client->isReady() || !m_modelLoaded) {
        ++m_usageRequest;
        m_meter->clearUsage();
        applyMeterWarning();
        return;
    }
    QJsonArray messages = effectiveMessages();
    const QString draft = m_input->toPlainText().trimmed();
    if (!draft.isEmpty()) messages.append(QJsonObject{{"role", "user"}, {"content", draft}});
    QJsonObject body;
    body["messages"] = messages;
    body["max_tokens"] = m_maxTokensSpin->value();
    const quint64 requestId = ++m_usageRequest;
    m_client->post("/inference/context-usage", body, [this, requestId](int status, const QJsonDocument &doc) {
        if (requestId != m_usageRequest) return;
        if (status == 200) {
            const QJsonObject usage = doc.object();
            m_meter->setUsage(usage["used"].toInt(), usage["reserved"].toInt(), usage["limit"].toInt(),
                              usage["exact"].toBool(true));
        } else {
            m_meter->clearUsage();
        }
        applyMeterWarning();
    });
}

// Mirrors the meter's warning in the status line (all styles except Off) without
// ever replacing an error, and removes it again once the warning goes away.
void ChatPanel::applyMeterWarning() {
    // Off shows no warning at all; the banner already is the warning when it is visible.
    const bool bannerShown = m_meter->style() == ContextMeter::Style::RingBanner &&
                             m_meter->level() == ContextMeter::Level::Critical;
    const QString warning = (m_meter->style() == ContextMeter::Style::Off || bannerShown)
                                ? QString() : m_meter->warningText();
    const bool ours = !m_shownWarning.isEmpty() && m_statusLabel->text() == m_shownWarning;
    if (warning.isEmpty()) {
        if (ours) setStatus("Ready.");
        m_shownWarning.clear();
        return;
    }
    if (m_streaming || m_modelOperation != ModelOperation::Idle) return;
    if (!ours && m_statusLabel->property("state").toString() == "error") return;
    setStatus(warning, "warn");
    m_shownWarning = warning;
}

void ChatPanel::updateStreamingBubble(const QString &text, bool showCursor) {
    if (!m_streamBubble) return;
    m_streamBubble->setStreaming(showCursor);   // before the text, so an unclosed <think> reads "Thinking…" only while it streams
    m_streamBubble->setText(text);
    m_streamBubble->setCursorVisible(showCursor && !QSettings().value("appearance/studio3ReducedMotion",false).toBool());
    const bool follow = m_followOutput;
    m_chatLayout->invalidate();
    m_chatLayout->activate();
    m_chatHost->updateGeometry();
    if (follow) scrollToBottom();
}

void ChatPanel::scrollToBottom() {
    QTimer::singleShot(0,this,[this]() {
        auto *bar = m_chatScroll->verticalScrollBar();
        bar->setValue(bar->maximum());
        m_followOutput = true;
    });
}

void ChatPanel::updateActions() {
    const bool busy = isBusy();
    const bool ready = m_client && m_client->isReady();
    m_previewMode->setEnabled(!busy);
    m_sendButton->setEnabled(m_modelLoaded && !busy && !m_input->toPlainText().trimmed().isEmpty());
    m_modelButton->setEnabled(ready && !busy);
    switch (m_modelOperation) {
    case ModelOperation::Loading: m_modelButton->setText("Loading…"); break;
    case ModelOperation::Unloading: m_modelButton->setText("Unloading…"); break;
    case ModelOperation::Idle: m_modelButton->setText(m_modelLoaded ? "Unload model" : "Load model"); break;
    }
    m_modelCombo->setEnabled(!busy);
    m_engineCombo->setEnabled(!busy);
    m_input->setReadOnly(m_preparing);
    refreshRows();
    if (busy != m_busyReported) {
        m_busyReported = busy;
        emit busyChanged(busy);
    }
}

void ChatPanel::setStatus(const QString &text, const QString &tone) {
    m_statusLabel->setText(text);
    applyStatusTone(m_statusLabel, tone);
}

QString ChatPanel::settingsJson() const {
    QJsonObject obj;
    obj["engine"] = m_engineCombo->currentIndex();
    obj["maxTokens"] = m_maxTokensSpin->value();
    obj["temperature"] = m_tempSpin->value();
    if (!m_knowledge) obj["rag"] = m_ragCheck->isChecked();
    obj["model"] = m_modelCombo->currentText();
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void ChatPanel::applySettingsJson(const QString &json) {
    QJsonParseError err;
    auto doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return;
    auto o = doc.object();
    if (o.contains("engine")) m_engineCombo->setCurrentIndex(o["engine"].toInt());
    if (o.contains("maxTokens")) m_maxTokensSpin->setValue(o["maxTokens"].toInt());
    if (o.contains("temperature")) m_tempSpin->setValue(o["temperature"].toDouble());
    if (!m_knowledge && o.contains("rag")) m_ragCheck->setChecked(o["rag"].toBool());
    if (o.contains("model")) {
        const QString m = o["model"].toString();
        const int idx = m_modelCombo->findText(m);
        if (idx >= 0) m_modelCombo->setCurrentIndex(idx);
        else m_modelCombo->setEditText(m);
    }
}
void ChatPanel::loadModel(const QString &id) {
    if (isBusy()) return;
    m_modelCombo->setCurrentText(id);
    onLoadModel();
}
