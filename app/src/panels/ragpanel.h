#pragma once

#include "panels/basepanel.h"

#include <QList>
class QLabel;
class QPushButton;
class QScrollArea;
class QSpinBox;
class QVBoxLayout;
class QWidget;
class RagDocCard;

// RAG panel: ingest documents, configure retrieval, and manage the local
// vector store (Phase 5.5.5: documents render as glass cards, config as a
// glass card). Backend RAG engine lands in Phase 6; UI skeleton here.
class RagPanel : public BasePanel {
    Q_OBJECT

public:
    explicit RagPanel(QWidget *parent = nullptr);

    QString settingsJson() const override;
    void applySettingsJson(const QString &json) override;

private slots:
    void onAddDocuments();
    void onRemoveDocument();
    void onClearAll();
    void onCardClicked(RagDocCard *card, bool ctrl);

private:
    void setupUi();
    void setStatus(const QString &text);
    void clearCards();
    void updateEmptyState();
    bool hasSelection() const;

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
    QLabel *m_statusLabel;
    QList<RagDocCard *> m_cards;
};