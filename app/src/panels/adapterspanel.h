#pragma once

#include "panels/basepanel.h"

#include <QList>

class QLabel;
class QScrollArea;
class QWidget;
class QVBoxLayout;
class AdapterCard;
class BackendClient;

// Adapter registry with per-row actions and a shared archive import toolbar.
class AdaptersPanel : public BasePanel {
    Q_OBJECT

public:
    explicit AdaptersPanel(QWidget *parent = nullptr);

    void setBackendClient(BackendClient *client);

signals:
    // Emitted when the active adapter changes (activate/clear/delete).
    void adapterActivated();

public slots:
    void refresh();

private slots:
    void onActivate(AdapterCard *card);
    void onDeleteAdapter(AdapterCard *card);
    void onExportAdapter(AdapterCard *card);
    void onImportAdapter();

private:
    void setupUi();
    // tone: "" neutral | "ok" | "error" | "warn" — drives #statusLabel state.
    void setStatus(const QString &text, const QString &tone = QString());
    void clearCards();
    void loadActiveAdapter();
    QWidget *makeCardHost();

    BackendClient *m_client;
    QScrollArea *m_scrollArea;
    QVBoxLayout *m_cardLayout;
    QWidget *m_cardHost;
    QWidget *m_emptyState;
    QLabel *m_statusLabel;
    QList<AdapterCard *> m_cards;
    QString m_activeAdapterId;
};
