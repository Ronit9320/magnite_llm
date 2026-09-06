#pragma once

#include "panels/basepanel.h"

class QListWidget;
class QPushButton;
class QSpinBox;
class QLabel;

// RAG panel: ingest documents, configure retrieval, and manage the local
// vector store. Backend RAG engine lands in Phase 6; UI skeleton here.
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

private:
    void setupUi();
    void setStatus(const QString &text);

    QListWidget *m_docList;
    QPushButton *m_addButton;
    QPushButton *m_removeButton;
    QPushButton *m_clearButton;
    QSpinBox *m_chunkSizeSpin;
    QSpinBox *m_chunkOverlapSpin;
    QSpinBox *m_topKSpin;
    QLabel *m_statusLabel;
};
