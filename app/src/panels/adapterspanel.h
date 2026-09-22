#pragma once

#include "panels/basepanel.h"

#include <QList>

class QLabel;
class QScrollArea;
class QWidget;
class QPushButton;
class QVBoxLayout;
class AdapterCard;
class BackendClient;

// Adapters panel: manage trained LoRA adapters as a glass card grid (Phase
// 5.5.4, design doc §8). Each AdapterCard owns its Activate/Export/Import/
// Delete buttons; activating hot-switches the adapter active in the Chat panel.
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
    QVBoxLayout *m_flowLayout;
    QWidget *m_cardHost;
    QWidget *m_emptyState;
    QLabel *m_statusLabel;
    QList<AdapterCard *> m_cards;
    QString m_activeAdapterId;
};