#include <QPlainTextEdit>
#include <QKeyEvent>
#include <QTextCursor>
#include "panels/chatpanel.h"

#include "backend_client.h"
#include "widgets/surface.h"
#include "widgets/starfield.h"
#include "panels/chatbubble.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
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
    , m_clearButton(nullptr)
    , m_statusLabel(nullptr)
    , m_streamBubble(nullptr)
    , m_streaming(false)
    , m_cursorTimer(nullptr)
    , m_cursorVisible(false)
{
    setHeaderSubtitle("Your model, connected to your knowledge.");
    setupUi();
}

void ChatPanel::setupUi() {
    auto *layout = contentLayout();
    auto *toolbar = new QHBoxLayout();
    m_modelCombo = new QComboBox(this);
    m_modelCombo->setEditable(true);
    m_modelCombo->setMinimumContentsLength(22);
    m_modelCombo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_modelCombo->setAccessibleName("Model");
    m_modelCombo->lineEdit()->setPlaceholderText("Choose a model or enter its ID");
    toolbar->addWidget(m_modelCombo, 1);
    auto *loadBtn = new QPushButton("Load model", this);
    toolbar->addWidget(loadBtn);
    auto *optionsBtn = new QPushButton("Model settings", this);
    optionsBtn->setObjectName("chatSettingsToggle");
    optionsBtn->setCheckable(true);
    toolbar->addWidget(optionsBtn);
    m_clearButton = new QPushButton("+  New chat", this);
    toolbar->addWidget(m_clearButton);
    layout->addLayout(toolbar);

    auto *controls = Surface::card(this);
    controls->parentWidget()->setObjectName("chatSettings");
    auto *fields = new QHBoxLayout();
    auto *engineField = new QVBoxLayout();
    m_engineCombo = new QComboBox(this);
    m_engineCombo->addItems({"Auto (global setting)", "Local (vLLM)", "Local (llama.cpp)", "Local (torch)", "Local (Ollama)"});
    Surface::field(engineField, "Engine", m_engineCombo);
    fields->addLayout(engineField, 2);
    m_maxTokensSpin = new QSpinBox(this);
    m_maxTokensSpin->setRange(1, 8192);
    m_maxTokensSpin->setValue(256);
    auto *tokens = new QVBoxLayout();
    Surface::field(tokens, "Response tokens", m_maxTokensSpin);
    fields->addLayout(tokens, 1);
    m_tempSpin = new QDoubleSpinBox(this);
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
        pill->setWordWrap(true);
        pills->addWidget(pill);
    }
    m_adapterPill->hide();
    auto *refreshBtn = new QPushButton("Refresh", this);
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
    m_chatHost = new QWidget(this);
    m_chatLayout = new QVBoxLayout(m_chatHost);
    m_chatLayout->setContentsMargins(4, 20, 12, 20);
    m_chatLayout->setSpacing(20);
    m_welcome = new QWidget(m_chatHost);
    auto *welcome = new QVBoxLayout(m_welcome);
    welcome->setSpacing(14);
    welcome->addStretch();
    auto *seal = new Starfield(m_welcome);
    seal->setProperty("kineticGlyph", true);
    seal->setFixedSize(44, 44);
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
        auto *button = new QPushButton(workspaceActions[i].label, m_welcome);
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
    welcome->addSpacing(6);
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
    m_ragCheck = new QCheckBox("Your knowledge", this);
    m_ragCheck->setToolTip("Include matching passages from documents in your Knowledge workspace.");
    actions->addWidget(m_ragCheck);
    actions->addStretch();
    m_stopButton = new QPushButton("Stop", this);
    m_stopButton->setEnabled(false);
    actions->addWidget(m_stopButton);
    m_sendButton = new QPushButton("Send  ↑", this);
    m_sendButton->setObjectName("primaryBtn");
    actions->addWidget(m_sendButton);
    composer->addLayout(actions);
    conversation->addWidget(composer->parentWidget());
    auto *shortcutHint = new QLabel("Enter to send · Shift+Enter for a new line", this);
    shortcutHint->setAlignment(Qt::AlignCenter);
    shortcutHint->setObjectName("cardMeta");
    conversation->addWidget(shortcutHint);
    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName("statusLabel");
    m_statusLabel->setWordWrap(true);
    m_statusLabel->setAlignment(Qt::AlignCenter);
    conversation->addWidget(m_statusLabel);
    connect(m_sendButton, &QPushButton::clicked, this, &ChatPanel::onSend);
    connect(m_stopButton, &QPushButton::clicked, this, &ChatPanel::onStop);
    connect(m_clearButton, &QPushButton::clicked, this, &ChatPanel::onClearChat);
    m_cursorTimer = new QTimer(this);
    m_cursorTimer->setInterval(500);
    connect(m_cursorTimer, &QTimer::timeout, this, [this]() {
        m_cursorVisible = !m_cursorVisible;
        if (m_streamBubble) m_streamBubble->setCursorVisible(m_cursorVisible);
    });
    setStatus("Choose and load a model to get started.");
}

bool ChatPanel::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_input && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
            && !(key->modifiers() & Qt::ShiftModifier)) {
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
            return;
        }
        const QJsonObject obj = doc.object();
        const QJsonObject engine = obj["engine"].toObject();
        if (engine.isEmpty()) {
            m_enginePill->setText("engine: none");
            m_modelPill->setText("model: —");
            m_adapterPill->setVisible(false);
            setStatus("No inference engine loaded. Load a model to begin.");
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

    setStatus("Loading model (this may download the GGUF file)...");
    m_sendButton->setEnabled(false);

    m_client->post("/inference/load", body, [this](int status, const QJsonDocument &doc) {
        if (status == 200) {
            const QJsonObject obj = doc.object();
            const QJsonObject engine = obj["engine"].toObject();
            setStatus(QString("Loaded: %1 | %2")
                          .arg(engine["description"].toString(),
                               obj["loaded_model"].toString()));
            m_sendButton->setEnabled(true);
            refreshEngineStatus();
            m_input->setFocus();
        } else {
            const QString err = doc.object().contains("detail")
                                    ? doc.object()["detail"].toString()
                                    : QString("HTTP %1").arg(status);
            setStatus("Load failed: " + err, "error");
            m_sendButton->setEnabled(true);
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
    const QString text = m_input->toPlainText().trimmed();
    if (text.isEmpty() || m_streaming) return;
    if (!m_client || !m_client->isReady()) {
        setStatus("Backend not connected.", "error");
        return;
    }

    m_welcome->hide();
    m_input->clear();

    // User bubble — right aligned.
    auto *userBubble = new ChatBubble(ChatBubble::User, text, m_chatHost);
    m_chatLayout->addWidget(userBubble, 0, Qt::AlignRight);
    // Assistant bubble — left aligned; streamed into below.
    m_streamBubble = new ChatBubble(ChatBubble::Assistant, QString(), m_chatHost);
    m_chatLayout->addWidget(m_streamBubble, 0, Qt::AlignLeft);
    ensureTrailingStretch();
    scrollToBottom();

    // Append to history
    m_history.append(QJsonObject{{"role", "user"}, {"content", text}});

    m_pendingResponse.clear();
    m_streamFailed = false;
    const quint64 requestId = ++m_requestId;
    m_streaming = true;
    m_cursorVisible = false;
    m_cursorTimer->start();
    m_sendButton->setEnabled(false);
    m_stopButton->setEnabled(true);

    QJsonObject body;
    QJsonArray messages;
    for (const QJsonObject &m : m_history) {
        messages.append(m);
    }
    body["messages"] = messages;
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
                m_history.append(QJsonObject{{"role", "assistant"}, {"content", m_pendingResponse}});
                m_streamBubble = nullptr;
                m_streaming = false;
                m_cursorTimer->stop();
                m_cursorVisible = false;
                m_sendButton->setEnabled(true);
                m_stopButton->setEnabled(false);
                setStatus("Ready.");
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
                if (!m_pendingResponse.isEmpty())
                    m_history.append(QJsonObject{{"role", "assistant"}, {"content", m_pendingResponse}});
                m_streamBubble = nullptr;
                m_streaming = false;
                m_cursorTimer->stop();
                m_cursorVisible = false;
                m_sendButton->setEnabled(true);
                m_stopButton->setEnabled(false);
            }
        });
}

void ChatPanel::onStop() {
    if (!m_streaming || !m_client) return;
    ++m_requestId;
    m_client->abortStream();
    updateStreamingBubble(m_pendingResponse, false);
    if (!m_pendingResponse.isEmpty()) {
        updateStreamingBubble(m_pendingResponse, false);
        m_history.append(QJsonObject{{"role", "assistant"}, {"content", m_pendingResponse}});
    }
    m_streamBubble = nullptr;
    m_streaming = false;
    m_cursorTimer->stop();
    m_cursorVisible = false;
    m_sendButton->setEnabled(true);
    m_stopButton->setEnabled(false);
    setStatus("Generation stopped.");
}

void ChatPanel::onClearChat() {
    if (m_streaming) onStop();
    m_history.clear();
    // Tear down streaming state.
    m_streaming = false;
    m_cursorTimer->stop();
    m_cursorVisible = false;
    m_streamBubble = nullptr;
    m_pendingResponse.clear();
    // Remove every bubble.
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
    m_sendButton->setEnabled(true);
    m_stopButton->setEnabled(false);
    setStatus("Chat cleared.");
}

void ChatPanel::updateStreamingBubble(const QString &text, bool showCursor) {
    if (!m_streamBubble) return;
    m_streamBubble->setText(text);
    m_streamBubble->setCursorVisible(showCursor);
    m_chatLayout->invalidate();
    m_chatLayout->activate();
    m_chatHost->updateGeometry();
    scrollToBottom();
}

void ChatPanel::scrollToBottom() {
    QScrollBar *sb = m_chatScroll->verticalScrollBar();
    sb->setValue(sb->maximum());
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
