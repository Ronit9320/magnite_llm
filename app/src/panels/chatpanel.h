#pragma once

#include "panels/basepanel.h"

#include <QList>
#include <QJsonObject>

class QComboBox;
class QLineEdit;
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
    void applySettingsJson(const QString &json) override;

public slots:
    void refreshEngineStatus();
    void loadModel(const QString &id);

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
    void ensureTrailingStretch();
    QString currentEngineName() const;

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
    QVBoxLayout *m_chatLayout;

    // Input row.
    QLineEdit *m_input;
    QPushButton *m_sendButton;
    QPushButton *m_stopButton;
    QPushButton *m_clearButton;
    QLabel *m_statusLabel;

    // Streaming state (presentation only — plumbing untouched).
    QList<QJsonObject> m_history;
    ChatBubble *m_streamBubble;
    QString m_pendingResponse;
    bool m_streaming;
    QTimer *m_cursorTimer;
    bool m_cursorVisible;
};