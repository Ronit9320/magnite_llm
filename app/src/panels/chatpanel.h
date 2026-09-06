#pragma once

#include "panels/basepanel.h"

#include <QList>
#include <QJsonObject>
#include <QTextBlock>

class QComboBox;
class QTextEdit;
class QLineEdit;
class QPushButton;
class QCheckBox;
class QLabel;
class QPlainTextEdit;
class QSpinBox;
class QDoubleSpinBox;
class BackendClient;

// Chat / Inference panel: converse with the active model.
class ChatPanel : public BasePanel {
    Q_OBJECT

public:
    explicit ChatPanel(QWidget *parent = nullptr);

    void setBackendClient(BackendClient *client);
    void refreshModels();

    QString settingsJson() const override;
    void applySettingsJson(const QString &json) override;

private slots:
    void onLoadModel();
    void onSend();
    void onStop();
    void onClearChat();

private:
    void setupUi();
    void setStatus(const QString &text, const QString &color = QString());
    void refreshEngineStatus();
    void updateStreamingBlock(const QString &text, bool showCursor);
    QString currentEngineName() const;

    BackendClient *m_client;
    QComboBox *m_modelCombo;
    QComboBox *m_engineCombo;
    QSpinBox *m_maxTokensSpin;
    QDoubleSpinBox *m_tempSpin;
    QCheckBox *m_ragCheck;
    QPlainTextEdit *m_conversation;
    QLineEdit *m_input;
    QPushButton *m_sendButton;
    QPushButton *m_stopButton;
    QPushButton *m_clearButton;
    QLabel *m_statusLabel;

    QList<QJsonObject> m_history;
    QTextBlock m_streamBlock;
    QString m_pendingResponse;
    bool m_streaming;
};
