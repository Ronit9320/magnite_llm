#pragma once

#include "panels/basepanel.h"
#include "panels/ragparams.h"

#include <optional>

#include <QList>
#include <QPointer>
#include <QJsonValue>
class QTimer;
class ComboBox;
class QLabel;
class QLineEdit;
class QNetworkReply;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QVBoxLayout;
class QWidget;
class BackendClient;
class RagDocCard;

// RAG panel: ingest documents, embed them locally, configure retrieval, and
// test queries against the vector store (Phase 6). The card grid, empty state
// and retrieval config card render documents returned by the backend.
class RagPanel : public BasePanel {
    Q_OBJECT

public:
    explicit RagPanel(QWidget *parent = nullptr);

    void setBackendClient(BackendClient *client);
    void refresh(bool preserveStatus = false);
    QJsonValue documentScope() const { return m_documentScope; }
    void setDocumentScope(const QJsonValue &scope);
    int enabledSourceCount() const;
    // Index these files (what "Add files…" does after its dialog); public for the UI checks.
    void ingestFiles(const QStringList &files);

signals:
    void sourceScopeChanged();

protected:
    void changeEvent(QEvent *event) override;

public:

    QString settingsJson() const override;

protected:
    void applySettingsJson(const QString &json) override;

private slots:
    void onAddDocuments();
    void onRemoveDocument();
    void onClearAll();
    void onCardClicked(RagDocCard *card, bool ctrl);
    void onTestQuery();
    void onConfigChanged();
    void onModelChosen(int index);

private:
    void setupUi();
    void setStatus(const QString &text, const QString &tone = QString());
    void clearCards();
    void updateEmptyState();
    bool hasSelection() const;
    QString selectedIds() const;
    void pushConfig();
    void applyStatus();
    void populatePicker();
    void revertPicker();
    void updateModelHint();
    QString modelWarning(const RagParams::Preset &preset) const;
    void startReindex(const RagParams::Preset &preset);
    void setMutating(bool on);
    void beginOperation(QNetworkReply *reply);
    void endOperation();
    void pollActivity();
    void showActivity(const QJsonObject &activity);
    void cancelOperations();

    BackendClient *m_client;
    QPushButton *m_addButton;
    QPushButton *m_removeButton;
    QPushButton *m_clearButton;
    QScrollArea *m_docScroll;
    QWidget *m_docHost;
    QVBoxLayout *m_docLayout;
    QWidget *m_emptyState;
    QLabel *m_countLabel = nullptr;
    QWidget *m_listActions = nullptr;
    QLabel *m_selectionHint = nullptr;
    QSpinBox *m_chunkSizeSpin;
    QSpinBox *m_chunkOverlapSpin;
    QSpinBox *m_topKSpin;
    ComboBox *m_modelPicker = nullptr;
    QLabel *m_modelHint = nullptr;
    QWidget *m_progressBox = nullptr;
    QLabel *m_progressLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QTimer *m_activityTimer = nullptr;
    QList<QPointer<QNetworkReply>> m_operations;  // in-flight ingest/reindex requests (abort = cancel)
    int m_runningOps = 0;
    bool m_cancelRequested = false;
    QLabel *m_modelInfo = nullptr;
    QLabel *m_chunkingInfo = nullptr;
    QLabel *m_searchInfo = nullptr;
    QLabel *m_clampHint = nullptr;
    QList<RagParams::Preset> m_presets;
    std::optional<RagParams::Active> m_active;   // what GET /rag/status last reported
    quint64 m_refreshRevision = 0;
    bool m_dirtyChunking = false, m_dirtyTopK = false;
    QLineEdit *m_queryEdit;
    QLabel *m_resultLabel;
    QLabel *m_statusLabel;
    QList<RagDocCard *> m_cards;
    bool m_loadingConfig;
    bool m_mutating = false;
    bool m_queryBusy = false;
    quint64 m_collectionRevision = 0;
    QPushButton *m_queryButton = nullptr;
    QJsonValue m_documentScope;
    QStringList m_knownDocIds;      // the documents the last listing showed, to tell new ones from narrowed-out ones
    QTimer *m_configTimer = nullptr;
};