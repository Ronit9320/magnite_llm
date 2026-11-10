#include "widgets/spinbox.h"
#include <QPlainTextEdit>
#include <QTime>
#include <QRandomGenerator>
#include <QKeyEvent>
#include <QTextCursor>
#include "panels/chatpanel.h"
#include "panels/chatstore.h"
#include "panels/greeting.h"

#include "backend_client.h"
#include "widgets/surface.h"
#include "widgets/studiocanvas.h"
#include "widgets/contextmeter.h"
#include "panels/chatbubble.h"
#include "panels/messagerow.h"

#include <QSettings>
#include "widgets/shortcuts.h"
#include <QStyle>
#include "widgets/toggleswitch.h"
#include "widgets/combobox.h"
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
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

ChatPanel::ChatPanel(QWidget *parent)
    : BasePanel("Chat", parent)
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
    setHeaderSubtitle("Your model, connected to your knowledge.");
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
    auto *layout = contentLayout();
    auto *toolbar = new QHBoxLayout();
    m_modelCombo = new ComboBox(this);
    m_modelCombo->setEditable(true);
    m_modelCombo->setMinimumContentsLength(22);
    m_modelCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_modelCombo->setAccessibleName("Model");
    m_modelCombo->lineEdit()->setPlaceholderText("Choose a model or enter its ID");
    toolbar->addWidget(m_modelCombo, 1);
    auto *loadBtn = m_loadButton = new Button("Load model", this);
    toolbar->addWidget(loadBtn);
    auto *optionsBtn = new Button("Model settings", this);
    optionsBtn->setObjectName("chatSettingsToggle");
    optionsBtn->setCheckable(true);
    toolbar->addWidget(optionsBtn);
    layout->addLayout(toolbar);

    auto *controls = Surface::card(this);
    controls->parentWidget()->setObjectName("chatSettings");
    auto *fields = new QHBoxLayout();
    auto *engineField = new QVBoxLayout();
    m_engineCombo = new ComboBox(this);
    m_engineCombo->addItems({"Auto (global setting)", "Local (vLLM)", "Local (llama.cpp)", "Local (torch)", "Local (Ollama)"});
    Surface::field(engineField, "Engine", m_engineCombo);
    fields->addLayout(engineField, 2);
    m_maxTokensSpin = new SpinBox(this);
    m_maxTokensSpin->setRange(1, 8192);
    m_maxTokensSpin->setValue(256);
    auto *tokens = new QVBoxLayout();
    Surface::field(tokens, "Response tokens", m_maxTokensSpin);
    fields->addLayout(tokens, 1);
    m_tempSpin = new DoubleSpinBox(this);
    m_tempSpin->setRange(0.0, 2.0);
    m_tempSpin->setValue(0.7);
    m_tempSpin->setSingleStep(0.05);
    m_tempSpin->setDecimals(2);
    auto *temperature = new QVBoxLayout();
    Surface::field(temperature, "Temperature", m_tempSpin);
    fields->addLayout(temperature, 1);
    controls->addLayout(fields);
    auto *pills = new QHBoxLayout();
    m_enginePill = new QLabel("engine: —", this);
    m_modelPill = new QLabel("model: —", this);
    m_adapterPill = new QLabel("adapter: —", this);
    for (auto *pill : {m_enginePill, m_modelPill, m_adapterPill}) {
        pill->setObjectName("chatPill");
        pill->setTextFormat(Qt::PlainText);
        pill->setWordWrap(true);
        pill->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
        pills->addWidget(pill);
    }
    m_adapterPill->hide();
    auto *refreshBtn = new Button("Refresh", this);
    pills->addWidget(refreshBtn);
    controls->addLayout(pills);
    layout->addWidget(controls->parentWidget());
    controls->parentWidget()->hide();
    connect(optionsBtn, &QPushButton::toggled, controls->parentWidget(), &QWidget::setVisible);
    connect(refreshBtn, &QPushButton::clicked, this, [this]() { refreshModels(); refreshEngineStatus(); });
    connect(loadBtn, &QPushButton::clicked, this, &ChatPanel::onLoadModel);

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
    layout->addLayout(centerRow, 1);
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
    auto *seal = new StudioCanvas(m_welcome);
    seal->setProperty("brandMark", true);
    seal->setFixedSize(64, 64);
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
        {"01 / YOUR MODEL\nPut it to the test", "Draft a reasoning prompt for your selected model."},
        {"02 / YOUR KNOWLEDGE\nAsk your documents", "Enable Knowledge and draft a question about your ingested documents."},
        {"03 / YOUR CONTROL\nShape the response", "Open engine, response length and temperature settings."}
    };
    for (int i = 0; i < 3; ++i) {
        auto *button = new Button(workspaceActions[i].label, m_welcome);
        button->setObjectName("promptSuggestion");
        button->setToolTip(workspaceActions[i].description);
        suggestions->addWidget(button);
        connect(button, &QPushButton::clicked, this, [this, optionsBtn, i]() {
            if (i == 2) {
                optionsBtn->setChecked(true);
                m_tempSpin->setFocus();
                return;
            }
            if (i == 1) m_ragCheck->setChecked(true);
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
    m_ragCheck = new ToggleSwitch("Your knowledge", this);
    m_ragCheck->setToolTip("Include matching passages from documents in your Knowledge workspace.");
    actions->addWidget(m_ragCheck);
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
    setStatus(client && client->isReady()
                  ? "Backend connected. Loading model list..."
                  : "Backend not connected.");
    refreshModels();
    refreshEngineStatus();
}

void ChatPanel::refreshModels() {
    if (!m_client || !m_client->isReady()) return;
    m_client->get("/models", [this](int status, const QJsonDocument &doc) {
        if (status != 200) return;
        const QJsonArray models = doc.array();
        m_modelCombo->blockSignals(true);
        const QString prev = m_modelCombo->currentText();
        m_modelCombo->clear();
        for (const QJsonValue &v : models) {
            const QJsonObject m = v.toObject();
            const QString id = m["id"].toString();
            if (!id.isEmpty()) m_modelCombo->addItem(id);
        }
        // Restore previous text if still present
        const int idx = m_modelCombo->findText(prev);
        if (idx >= 0) m_modelCombo->setCurrentIndex(idx);
        else m_modelCombo->setEditText(prev);
        m_modelCombo->blockSignals(false);
    });
}

void ChatPanel::refreshEngineStatus() {
    if (!m_client || !m_client->isReady()) {
        setStatus("Backend not connected.");
        return;
    }
    m_client->get("/inference/status", [this](int status, const QJsonDocument &doc) {
        if (status != 200) {
            m_enginePill->setText("engine: —");
            m_modelPill->setText("model: —");
            m_adapterPill->setVisible(false);
            setStatus("Inference status unavailable.");
            m_modelLoaded = false;
            refreshContextUsage();
            return;
        }
        const QJsonObject obj = doc.object();
        const QJsonObject engine = obj["engine"].toObject();
        if (engine.isEmpty()) {
            m_enginePill->setText("engine: none");
            m_modelPill->setText("model: —");
            m_adapterPill->setVisible(false);
            setStatus("No inference engine loaded. Load a model to begin.");
            m_modelLoaded = false;
            refreshContextUsage();
            return;
        }
        m_enginePill->setText(QString("engine: %1")
                                  .arg(engine["description"].toString()));
        m_modelPill->setText(QString("model: %1")
                                 .arg(obj["loaded_model"].toString()));

        const QString adapter = obj["active_adapter"].toString();
        if (adapter.isEmpty()) {
            m_adapterPill->setVisible(false);
        } else {
            m_adapterPill->setText(QString("adapter: %1").arg(adapter));
            m_adapterPill->setVisible(true);
        }
        setStatus("Engine ready.");
        m_modelLoaded = true;
        refreshContextUsage();
    });
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
    if (m_loading || m_streaming) return;
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
    QJsonObject body;
    body["model"] = modelRef;
    if (!engineName.isEmpty()) body["engine"] = engineName;

    m_loading = true;
    updateActions();
    setStatus("Loading model… First use may download model files.");

    m_client->post("/inference/load", body, [this](int status, const QJsonDocument &doc) {
        m_loading = false;
        updateActions();
        if (status == 200) {
            const QJsonObject obj = doc.object();
            const QJsonObject engine = obj["engine"].toObject();
            setStatus(QString("Loaded: %1 | %2")
                          .arg(engine["description"].toString(),
                               obj["loaded_model"].toString()));
            refreshEngineStatus();
            m_input->setFocus();
        } else {
            const QString err = doc.object().contains("detail")
                                    ? doc.object()["detail"].toString()
                                    : QString("HTTP %1").arg(status);
            setStatus("Load failed: " + err, "error");
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
    if (m_loading || m_preparing) return;
    const QString text = m_input->toPlainText().trimmed();
    if (text.isEmpty() || m_streaming) return;
    if (anyEditing()) {
        setStatus("Finish or cancel your edit first.", "warn");
        return;
    }
    beginSend(PendingSend{PendingSend::NewMessage, ChatTree::kRoot, text});
}

bool ChatPanel::beginSend(const PendingSend &send) {
    if (m_loading || m_preparing || m_streaming) return false;
    if (!m_client || !m_client->isReady()) {
        setStatus("Backend not connected.", "error");
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
    const bool busy = m_streaming || m_preparing || m_loading;
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
    if (m_streaming || m_preparing || m_loading || anyEditing()) return;
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
    if (m_streaming || m_preparing || m_loading) return;
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
    if (row) row->setNode(reply);
    markDirty();
    return true;
}

void ChatPanel::onRetry(ChatTree::NodeId reply) {
    if (m_streaming || m_preparing || m_loading || anyEditing()) return;
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
    body["rag"] = QJsonObject{{"enabled", m_ragCheck->isChecked()}};

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
    // "New chat": the chat on screen stays saved, a blank one takes its place.
    stopActivity();
    flushSave();
    m_chatId.clear();
    resetConversation();
    setStatus("New chat.");
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

void ChatPanel::setChatStore(ChatStore *store) {
    if (m_store == store) return;
    flushSave();
    m_store = store;
    // Whatever is on screen belongs to the previous store (or to none): it is not saved into the new one.
    m_saveDirty = false;
    m_saveTimer->stop();
    m_chatId.clear();
}

bool ChatPanel::isBusy() const { return m_streaming || m_preparing || m_loading || anyEditing(); }

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
    if (id == m_chatId) return true;
    const std::optional<ChatTree> loaded = m_store->load(id);
    if (!loaded) {
        setStatus("That chat could not be opened.", "error");
        return false;
    }
    flushSave();
    resetConversation(false);
    m_tree = *loaded;
    m_chatId = id;
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
    setStatus("Chat deleted.");
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
    if (m_streaming || m_loading) return;
    if (!ours && m_statusLabel->property("state").toString() == "error") return;
    setStatus(warning, "warn");
    m_shownWarning = warning;
}

void ChatPanel::updateStreamingBubble(const QString &text, bool showCursor) {
    if (!m_streamBubble) return;
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
    m_sendButton->setEnabled(!m_streaming && !m_loading && !m_preparing && !anyEditing() && !m_input->toPlainText().trimmed().isEmpty());
    m_loadButton->setEnabled(!m_streaming && !m_loading && !m_preparing);
    m_loadButton->setText(m_loading ? "Loading…" : "Load model");
    m_modelCombo->setEnabled(!m_streaming && !m_loading && !m_preparing);
    m_engineCombo->setEnabled(!m_streaming && !m_loading && !m_preparing);
    m_input->setReadOnly(m_preparing);
    refreshRows();
    const bool busy = isBusy();
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
    obj["rag"] = m_ragCheck->isChecked();
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
    if (o.contains("rag")) m_ragCheck->setChecked(o["rag"].toBool());
    if (o.contains("model")) {
        const QString m = o["model"].toString();
        const int idx = m_modelCombo->findText(m);
        if (idx >= 0) m_modelCombo->setCurrentIndex(idx);
        else m_modelCombo->setEditText(m);
    }
}
void ChatPanel::loadModel(const QString &id) {
    m_modelCombo->setCurrentText(id);
    onLoadModel();
}
