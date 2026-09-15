#include "panels/chatpanel.h"

#include "backend_client.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QTextCursor>
#include <QVBoxLayout>

ChatPanel::ChatPanel(QWidget *parent)
    : BasePanel("Chat", parent)
    , m_client(nullptr)
    , m_modelCombo(nullptr)
    , m_engineCombo(nullptr)
    , m_maxTokensSpin(nullptr)
    , m_tempSpin(nullptr)
    , m_ragCheck(nullptr)
    , m_conversation(nullptr)
    , m_input(nullptr)
    , m_sendButton(nullptr)
    , m_stopButton(nullptr)
    , m_clearButton(nullptr)
    , m_statusLabel(nullptr)
    , m_streaming(false)
{
    setHeaderSubtitle("Converse with the active model.");
    setupUi();
}

void ChatPanel::setupUi() {
    auto *layout = contentLayout();

    // Row 1: model + engine + load
    auto *ctrlRow1 = new QHBoxLayout();
    m_modelCombo = new QComboBox(this);
    m_modelCombo->setEditable(true);
    m_modelCombo->setToolTip("Registered model id, or type a HuggingFace repo id with a GGUF file.");
    m_modelCombo->setSizeAdjustPolicy(QComboBox::AdjustToContents);

    m_engineCombo = new QComboBox(this);
    m_engineCombo->addItems({"Auto (detect hardware)", "Local (vLLM)", "Local (llama.cpp)"});
    m_engineCombo->setCurrentIndex(0);

    auto *loadBtn = new QPushButton("Load Model", this);
    loadBtn->setObjectName("primaryBtn");
    connect(loadBtn, &QPushButton::clicked, this, &ChatPanel::onLoadModel);

    ctrlRow1->addWidget(new QLabel("Model:", this));
    ctrlRow1->addWidget(m_modelCombo, 1);
    ctrlRow1->addWidget(new QLabel("Engine:", this));
    ctrlRow1->addWidget(m_engineCombo);
    ctrlRow1->addWidget(loadBtn);
    layout->addLayout(ctrlRow1);

    // Row 2: generation params + RAG
    auto *ctrlRow2 = new QHBoxLayout();
    m_maxTokensSpin = new QSpinBox(this);
    m_maxTokensSpin->setRange(1, 8192);
    m_maxTokensSpin->setValue(256);
    m_maxTokensSpin->setPrefix("Tokens: ");

    m_tempSpin = new QDoubleSpinBox(this);
    m_tempSpin->setRange(0.0, 2.0);
    m_tempSpin->setValue(0.7);
    m_tempSpin->setSingleStep(0.05);
    m_tempSpin->setDecimals(2);
    m_tempSpin->setPrefix("Temp: ");

    m_ragCheck = new QCheckBox("Use RAG", this);
    m_ragCheck->setToolTip("Phase 6: inject retrieved context.");

    auto *statusBtn = new QPushButton("Refresh Status", this);
    statusBtn->setObjectName("secondaryBtn");
    connect(statusBtn, &QPushButton::clicked, this, &ChatPanel::refreshEngineStatus);

    ctrlRow2->addWidget(m_maxTokensSpin);
    ctrlRow2->addWidget(m_tempSpin);
    ctrlRow2->addSpacing(8);
    ctrlRow2->addWidget(m_ragCheck);
    ctrlRow2->addStretch();
    ctrlRow2->addWidget(statusBtn);
    layout->addLayout(ctrlRow2);

    // Conversation
    m_conversation = new QPlainTextEdit(this);
    m_conversation->setReadOnly(true);
    m_conversation->setPlaceholderText(
        "Load a model, then type a message to begin.\n"
        "Inference supports HuggingFace GGUF repos directly.");
    layout->addWidget(m_conversation, 1);

    // Input row
    auto *inputRow = new QHBoxLayout();
    m_input = new QLineEdit(this);
    m_input->setPlaceholderText("Type a message...");
    m_sendButton = new QPushButton("Send", this);
    m_sendButton->setObjectName("primaryBtn");
    m_stopButton = new QPushButton("Stop", this);
    m_stopButton->setObjectName("destructiveBtn");
    m_stopButton->setEnabled(false);
    m_clearButton = new QPushButton("Clear Chat", this);
    m_clearButton->setObjectName("secondaryBtn");
    inputRow->addWidget(m_input, 1);
    inputRow->addWidget(m_sendButton);
    inputRow->addWidget(m_stopButton);
    inputRow->addWidget(m_clearButton);
    layout->addLayout(inputRow);

    // Status
    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName("statusLabel");
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);

    connect(m_sendButton, &QPushButton::clicked, this, &ChatPanel::onSend);
    connect(m_input, &QLineEdit::returnPressed, this, &ChatPanel::onSend);
    connect(m_stopButton, &QPushButton::clicked, this, &ChatPanel::onStop);
    connect(m_clearButton, &QPushButton::clicked, this, &ChatPanel::onClearChat);

    setStatus("Load a model to begin.");
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
            setStatus("Inference status unavailable.");
            return;
        }
        const QJsonObject obj = doc.object();
        const QJsonObject engine = obj["engine"].toObject();
        if (engine.isEmpty()) {
            setStatus("No inference engine loaded. Load a model to begin.");
            return;
        }
        QString out = QString("Engine: %1 | Model: %2")
                             .arg(engine["description"].toString(),
                                  obj["loaded_model"].toString());
        const QString adapter = obj["active_adapter"].toString();
        if (!adapter.isEmpty()) {
            out += QString(" | Adapter: %1").arg(adapter);
        }
        setStatus(out);
    });
}

QString ChatPanel::currentEngineName() const {
    switch (m_engineCombo->currentIndex()) {
        case 1: return "vllm";
        case 2: return "llamacpp";
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
            m_input->setFocus();
        } else {
            const QString err = doc.object().contains("detail")
                                    ? doc.object()["detail"].toString()
                                    : QString("HTTP %1").arg(status);
            setStatus("Load failed: " + err, "error");
        }
    });
}

void ChatPanel::onSend() {
    const QString text = m_input->text().trimmed();
    if (text.isEmpty() || m_streaming) return;
    if (!m_client || !m_client->isReady()) {
        setStatus("Backend not connected.", "error");
        return;
    }

    m_input->clear();
    m_conversation->appendPlainText(QString("[%1] %2").arg("You", text));

    // Append to history
    m_history.append(QJsonObject{{"role", "user"}, {"content", text}});

    // Prepare streaming block
    {
        QTextCursor cur(m_conversation->document());
        cur.movePosition(QTextCursor::End);
        cur.insertBlock();
        m_streamBlock = cur.block();
    }
    m_pendingResponse.clear();
    m_streaming = true;
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

    setStatus("Generating...");
    m_client->streamPost(
        "/inference/chat",
        body,
        // onEvent
        [this](const QJsonObject &event) {
            const QString delta = event.value("delta").toString();
            if (!delta.isEmpty()) {
                m_pendingResponse += delta;
                updateStreamingBlock(m_pendingResponse, true);
            }
            if (event.value("done").toBool()) {
                updateStreamingBlock(m_pendingResponse, false);
                m_history.append(QJsonObject{{"role", "assistant"}, {"content", m_pendingResponse}});
                m_streamBlock = QTextBlock();
                m_streaming = false;
                m_sendButton->setEnabled(true);
                m_stopButton->setEnabled(false);
                setStatus("Ready.");
            }
        },
        // onDone
        [this](int statusCode) {
            if (statusCode == 0) return; // aborted
            if (statusCode != 200) {
                const QString err = m_pendingResponse.isEmpty()
                                       ? QString("HTTP %1").arg(statusCode)
                                       : m_pendingResponse;
                setStatus("Generation error: " + err, "error");
            }
            // If not already finalized by done event
            if (m_streaming) {
                updateStreamingBlock(m_pendingResponse, false);
                if (!m_pendingResponse.isEmpty())
                    m_history.append(QJsonObject{{"role", "assistant"}, {"content", m_pendingResponse}});
                m_streamBlock = QTextBlock();
                m_streaming = false;
                m_sendButton->setEnabled(true);
                m_stopButton->setEnabled(false);
            }
        });
}

void ChatPanel::onStop() {
    if (!m_streaming || !m_client) return;
    m_client->abortStream();
    if (!m_pendingResponse.isEmpty())
        updateStreamingBlock(m_pendingResponse, false);
    m_streamBlock = QTextBlock();
    m_streaming = false;
    m_sendButton->setEnabled(true);
    m_stopButton->setEnabled(false);
    setStatus("Generation stopped.");
}

void ChatPanel::onClearChat() {
    m_history.clear();
    m_conversation->clear();
    m_streamBlock = QTextBlock();
    m_pendingResponse.clear();
    m_streaming = false;
    m_sendButton->setEnabled(true);
    m_stopButton->setEnabled(false);
    setStatus("Chat cleared.");
}

void ChatPanel::updateStreamingBlock(const QString &text, bool showCursor) {
    if (!m_streamBlock.isValid()) return;
    QTextCursor cur(m_streamBlock);
    cur.select(QTextCursor::BlockUnderCursor);
    cur.insertText(showCursor ? (text + QStringLiteral(" ")) : text);
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
