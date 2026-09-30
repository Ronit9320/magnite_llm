#pragma once

#include "panels/basepanel.h"

#include <QList>
class QTimer;
class QLabel;
class QLineEdit;
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

private:
    void setupUi();
    void setStatus(const QString &text, const QString &tone = QString());
    void clearCards();
    void updateEmptyState();
    bool hasSelection() const;
    QString selectedIds() const;
    void pushConfig();

    BackendClient *m_client;
    QPushButton *m_addButton;
    QPushButton *m_removeButton;
    QPushButton *m_clearButton;
    QScrollArea *m_docScroll;
    QWidget *m_docHost;
    QVBoxLayout *m_docLayout;
    QWidget *m_emptyState;
    QSpinBox *m_chunkSizeSpin;
    QSpinBox *m_chunkOverlapSpin;
    QSpinBox *m_topKSpin;
    QLineEdit *m_queryEdit;
    QLabel *m_resultLabel;
    QLabel *m_statusLabel;
    QList<RagDocCard *> m_cards;
    bool m_loadingConfig;
    bool m_mutating = false;
    bool m_queryBusy = false;
    quint64 m_collectionRevision = 0;
    QPushButton *m_queryButton = nullptr;
    QTimer *m_configTimer = nullptr;
};