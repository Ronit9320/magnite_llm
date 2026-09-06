#pragma once

#include "panels/basepanel.h"

class QTableWidget;
class QPushButton;
class QLabel;
class BackendClient;

// Adapters panel: manage trained LoRA adapters and switch the active one.
// Hot-switching (Activate) lands in Phase 5; list/delete are live now.
class AdaptersPanel : public BasePanel {
    Q_OBJECT

public:
    explicit AdaptersPanel(QWidget *parent = nullptr);

    void setBackendClient(BackendClient *client);

public slots:
    void refresh();

private slots:
    void onActivate();
    void onDeleteAdapter();

private:
    void setupUi();
    void setStatus(const QString &text);
    QString selectedId() const;

    BackendClient *m_client;
    QTableWidget *m_table;
    QPushButton *m_activateButton;
    QPushButton *m_deleteButton;
    QLabel *m_statusLabel;
};