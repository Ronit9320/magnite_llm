#pragma once

#include "panels/basepanel.h"

#include <QList>
#include <QPointer>
#include <QJsonArray>
#include <QJsonObject>

class StudioCanvas;
class QComboBox;
class QPlainTextEdit;
class QPushButton;
class QCheckBox;
class QLabel;
class QScrollArea;
class QSpinBox;
class QDoubleSpinBox;
class QVBoxLayout;
class QTimer;
class BackendClient;
class ChatBubble;
class ContextMeter;
class QNetworkReply;

// Chat / Inference panel: converse with the active model.
// Presentation: real chat bubbles (Variant B) in a scroll area, with a pinned
// engine/model/adapter pill row at the top. All streaming plumbing is untouched.
class ChatPanel : public BasePanel {
    Q_OBJECT

public:
    explicit ChatPanel(QWidget *parent = nullptr);

    void setBackendClient(BackendClient *client);
    void refreshModels();

    QString settingsJson() const override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void applySettingsJson(const QString &json) override;

public slots:
    void refreshEngineStatus();
    void loadModel(const QString &id);
    // Re-read the chat/meterStyle and chat/autoCompactPercent settings and apply them.
    void reloadContextSettings();
    // Ask the backend how full the context is (history + typed draft + reserved reply).
    void refreshContextUsage();

private slots:
    void onLoadModel();
    void onSend();
    void onStop();
    void onClearChat();

private:
    void setupUi();
    void setStatus(const QString &text, const QString &tone = QString());
    void updateStreamingBubble(const QString &text, bool showCursor);
    void scrollToBottom();
    void updateActions();
    void ensureTrailingStretch();
    QString currentEngineName() const;
    // What is sent to the model: the compaction summary (if any) plus the uncompacted history.
    QJsonArray effectiveMessages() const;
    void applyMeterWarning();
    // Send flow: onSend decides whether to measure/compact first, startSend streams the reply.
    void startSend(const QString &text);
    void measureThenSend(const QString &text);
    void compactThenSend(const QString &text);
    void cancelPreparing();
    void addContextDivider(const QString &text);

    BackendClient *m_client;
    QComboBox *m_modelCombo;
    QComboBox *m_engineCombo;
    QSpinBox *m_maxTokensSpin;
    QDoubleSpinBox *m_tempSpin;
    QCheckBox *m_ragCheck;

    // Pinned indicator pills (design doc §8).
    QLabel *m_enginePill;
    QLabel *m_modelPill;
    QLabel *m_adapterPill;

    // Chat bubble area.
    QScrollArea *m_chatScroll;
    QWidget *m_chatHost;
    QLabel *m_emptyConversation = nullptr;
    QWidget *m_welcome = nullptr;
    QVBoxLayout *m_chatLayout;

    // Input row.
    QPlainTextEdit *m_input;
    QPushButton *m_sendButton;
    QPushButton *m_loadButton = nullptr;
    bool m_loading = false;
    bool m_followOutput = true;
    QPushButton *m_stopButton;
    QPushButton *m_clearButton;
    QLabel *m_statusLabel;
    StudioCanvas *m_activity = nullptr;

    // Streaming state (presentation only — plumbing untouched).
    QList<QJsonObject> m_history;

    // Context meter and compaction. m_history is never trimmed (the transcript stays);
    // `covered` leading messages are represented by `summary` when sending.
    struct Compaction {
        QString summary;
        int covered = 0;
    } m_compaction;
    ContextMeter *m_meter = nullptr;
    QTimer *m_usageTimer = nullptr;
    quint64 m_usageRequest = 0;
    bool m_modelLoaded = false;
    int m_autoCompactPercent = 85;
    bool m_preparing = false;          // measuring or compacting before a send
    quint64 m_prepareRequest = 0;
    QPointer<QNetworkReply> m_prepareReply;   // the usage/compact request being awaited
    static constexpr int kKeepRecent = 4;   // messages kept verbatim when compacting
    QString m_shownWarning;
    ChatBubble *m_streamBubble;
    QString m_pendingResponse;
    bool m_streaming;
    bool m_streamFailed = false;
    quint64 m_requestId = 0;
    QTimer *m_cursorTimer;
    bool m_cursorVisible;
};
