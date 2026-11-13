#pragma once

#include "panels/basepanel.h"
#include "panels/chatbubble.h"
#include "panels/chattree.h"

#include <functional>
#include <optional>

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
class QHBoxLayout;
class QTimer;
class BackendClient;
class ChatBubble;
class MessageRow;
class ContextMeter;
class ChatStore;
class RagPanel;
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
    void setKnowledgePanel(RagPanel *panel);
    void showKnowledge();

    QString settingsJson() const override;

    // Saved chats. Without a store (the default) nothing is saved and nothing can be opened.
    void setChatStore(ChatStore *store);
    QString currentChatId() const { return m_chatId; }   // empty for a blank, unsaved chat
    // Streaming, preparing a send, loading a model, or a message editor open.
    bool isBusy() const;
    // Writes any pending change now.
    bool flushSave();
    // Test hook: the hour the greeting uses (default: the clock). An empty function restores the clock.
    void setHourProviderForTest(std::function<int()> provider) { m_hourProvider = std::move(provider); }
    // Open a saved chat. False, changing nothing, when busy, unknown or unreadable.
    bool openChat(const QString &id);
    // False for an empty title, an unknown chat, or the open chat while busy.
    bool renameChat(const QString &id, const QString &title);
    // False for an unknown chat, or the open chat while busy. Deleting the open chat leaves a blank one.
    bool deleteChat(const QString &id);

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

signals:
    void currentChatChanged(const QString &id);   // "" when a blank chat is shown
    void busyChanged(bool busy);

private slots:
    void onLoadModel();
    void onSend();
    void onStop();
    void onClearChat();
    void redrawForTest();   // test hook: redraw the transcript from the conversation tree
    void onEditRequested(ChatTree::NodeId node);
    void onVersionStep(ChatTree::NodeId node, int delta);
    void onRetry(ChatTree::NodeId reply);
    void onEditSaved(ChatTree::NodeId node, const QString &text);

private:
    // What a send is about to do: send a new message at the end of the conversation, replace an
    // earlier user message (Edit), or replace the latest reply (Retry).
    struct PendingSend {
        enum Kind { NewMessage, Edit, Retry } kind = NewMessage;
        ChatTree::NodeId anchor = ChatTree::kRoot;   // Edit: the message being replaced; Retry: the reply being replaced
        QString text;                                // NewMessage/Edit: the user text; Retry: the user message's text
    };

    void setupUi();
    void updateKnowledgeContext();
    void restoreKnowledge();
    void addCitations(MessageRow *row, const QJsonArray &sources);
    // Shows the blank chat's welcome with a fresh greeting (a different wording from the last one).
    void showBlankChat();
    // Stops whatever is streaming or being prepared.
    void stopActivity();
    // Empties the conversation and every piece of send state, and shows the blank chat (with a fresh greeting when showBlank).
    void resetConversation(bool showBlank = true);
    // Call after any change to the tree: the first change of a new chat saves at once, later ones shortly after.
    void markDirty();
    void setStatus(const QString &text, const QString &tone = QString());
    void updateStreamingBubble(const QString &text, bool showCursor);
    void scrollToBottom();
    void updateActions();
    void ensureTrailingStretch();
    QString currentEngineName() const;
    // What is sent to the model: the compaction summary (if any) plus the uncompacted messages,
    // over the first `pathCount` messages of the active path (-1 = all of it).
    QJsonArray effectiveMessages(int pathCount = -1) const;
    // Path messages that come before the message a send adds or replaces.
    int prefixCount(const PendingSend &send) const;
    // Entry point for every send (new message, edit, retry). Returns false, doing nothing, when a
    // send cannot start now (busy, or the backend is not connected).
    bool beginSend(const PendingSend &send);
    MessageRow *makeRow(ChatBubble::Role role, ChatTree::NodeId node, const QString &text);
    MessageRow *rowForNode(ChatTree::NodeId node) const;
    bool anyEditing() const;
    // Enables or disables the per-message actions to match what the panel is doing.
    void refreshRows();
    // Adds the finished reply (if it has any text) under the message it answers. Returns whether a
    // reply was added; a retry that produced nothing leaves the previous reply on screen instead.
    bool commitReply();
    void applyMeterWarning();
    // Send flow: onSend decides whether to measure/compact first, startSend streams the reply.
    void startSend(const PendingSend &send);
    void measureThenSend(const PendingSend &send);
    void compactThenSend(const PendingSend &send);
    void cancelPreparing();
    void addContextDivider(const QString &text);
    // Draws the first `pathCount` messages of the active path (-1 = all) as rows, each preceded by
    // its notes (compaction dividers), then `trailingNotes`. The streaming reply row is added by the caller.
    void renderTranscript(int pathCount = -1, const QStringList &trailingNotes = QStringList());

    BackendClient *m_client;
    QComboBox *m_modelCombo;
    QComboBox *m_engineCombo;
    QSpinBox *m_maxTokensSpin;
    QDoubleSpinBox *m_tempSpin;
    QCheckBox *m_ragCheck;
    QPushButton *m_knowledgeToggle = nullptr;
    QPushButton *m_knowledgeContext = nullptr;
    QWidget *m_knowledgeDrawer = nullptr;
    QHBoxLayout *m_workspace = nullptr;
    RagPanel *m_knowledge = nullptr;
    bool m_restoringKnowledge = false;

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
    QLabel *m_statusLabel;
    StudioCanvas *m_activity = nullptr;

    // The conversation: a tree of messages whose active path is what is on screen and what is
    // sent. Compaction snapshots (`covered` leading messages represented by `summary`) live on
    // the tree nodes, so the transcript itself is never trimmed.
    ChatTree m_tree;
    std::function<int()> m_hourProvider;   // test hook; empty = the clock
    QString m_greeting;                    // the greeting on the blank chat, and the name it was made for
    QString m_greetingName;
    ChatStore *m_store = nullptr;
    QString m_chatId;                    // the saved chat on screen; empty until its first message
    QTimer *m_saveTimer = nullptr;
    bool m_saveDirty = false;
    bool m_busyReported = false;
    ChatTree::NodeId m_replyParent = ChatTree::kRoot;   // the user message the streaming reply answers
    // While a retry streams, only this many path messages count (the user message, not the reply
    // being replaced); -1 otherwise.
    int m_pathLimit = -1;
    // A retry's compaction waits here (not on the tree) until its reply has text, so a failed or
    // stopped retry changes nothing; it is then stored on the new reply.
    std::optional<ChatTree::Compaction> m_retryCompaction;
    bool m_streamHadReply = false;   // the retry replaces an existing reply (false: first reply to a message)
    PendingSend::Kind m_streamKind = PendingSend::NewMessage;   // what the streaming reply belongs to
    PendingSend m_activeSend;                                   // the send being measured or compacted
    std::optional<ChatTree::Compaction> m_pendingCompaction;   // compaction result not yet attached to a message
    QStringList m_pendingNotes;                                // divider texts not yet attached to a message
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
    QPointer<MessageRow> m_streamRow;   // the row holding the streaming reply
    QString m_pendingResponse;
    QJsonArray m_pendingSources;
    bool m_streaming;
    bool m_streamFailed = false;
    quint64 m_requestId = 0;
    QTimer *m_cursorTimer;
    bool m_cursorVisible;
};
