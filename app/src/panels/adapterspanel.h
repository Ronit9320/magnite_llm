#pragma once

#include "panels/basepanel.h"

class QTableWidget;
class QPushButton;
class QLabel;
class BackendClient;

// Adapters panel: manage trained LoRA adapters, export/import them, and
// hot-switch the adapter active in the Chat panel.
class AdaptersPanel : public BasePanel {
    Q_OBJECT

public:
    explicit AdaptersPanel(QWidget *parent = nullptr);

    void setBackendClient(BackendClient *client);

    QString activeAdapterId() const { return m_activeAdapterId; }

signals:
    // Emitted when the active adapter changes (activate/clear/delete).
    void adapterActivated();

public slots:
    void refresh();

private slots:
    void onActivate();
    void onDeleteAdapter();
    void onExportAdapter();
    void onImportAdapter();

private:
    void setupUi();
    // tone: "" neutral | "ok" | "error" | "warn" — drives #statusLabel state.
    void setStatus(const QString &text, const QString &tone = QString());
    QString selectedId() const;
    void loadActiveAdapter();

    BackendClient *m_client;
    QTableWidget *m_table;
    QPushButton *m_activateButton;
    QPushButton *m_exportButton;
    QPushButton *m_importButton;
    QPushButton *m_deleteButton;
    QLabel *m_statusLabel;
    QString m_activeAdapterId;
};