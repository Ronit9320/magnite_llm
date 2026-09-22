#pragma once

#include "panels/basepanel.h"

#include <QJsonArray>
#include <QList>

class QLineEdit;
class QPushButton;
class QScrollArea;
class QComboBox;
class QLabel;
class QTimer;
class QWidget;
class FlowLayout;
class ModelCard;
class OverviewTile;
class BackendClient;

// Models panel: browse local models, search HuggingFace, download, and delete.
// Phase 5.5.3: the old QTableWidget is a scrollable grid of glass ModelCards.
class ModelsPanel : public BasePanel {
    Q_OBJECT

public:
    explicit ModelsPanel(QWidget *parent = nullptr);

    void setBackendClient(BackendClient *client);

    QString settingsJson() const override;
    void applySettingsJson(const QString &json) override;

signals:
    void modelsChanged();
    void loadRequested(const QString &id);
    void workspaceRequested(int panel);

public slots:
    void refresh();

private slots:
    void onSearch();
    void onViewChanged(int index);
    void onDownload();
    void onDeleteModel();
    void onPullPoll();

private:
    void setupUi();
    void clearCards();
    // tone: "" neutral | "ok" | "error" | "warn" — drives #statusLabel state.
    void setStatus(const QString &text, const QString &tone = QString());
    void switchToLocalView();
    void showSearchResults(const QJsonArray &results);
    void showLocalModels(const QJsonArray &models);
    void addCards(const QJsonArray &items, bool isHub);
    void startPullPolling(const QString &jobId);
    void onCardClicked(ModelCard *card);
    void updateActionButtons();
    bool isLocalView() const;
    QString selectedId() const;
    ModelCard *cardById(const QString &id) const;
    QWidget *makeCardHost();
    // Polls /inference/status, /adapters, /rag/documents and /training/jobs and
    // repaints the four live-state tiles at the top of the panel.
    void refreshOverview();

    BackendClient *m_client;
    QLineEdit *m_searchEdit;
    QPushButton *m_searchButton;
    QPushButton *m_refreshButton;
    QPushButton *m_downloadButton;
    QPushButton *m_deleteButton;
    QComboBox *m_viewCombo;
    QScrollArea *m_scrollArea;
    FlowLayout *m_flowLayout;
    QWidget *m_cardHost;
    QLabel *m_statusLabel;
    QLabel *m_emptyRack = nullptr;
    QList<OverviewTile *> m_overviewTiles;
    QTimer *m_pullTimer;
    QTimer *m_overviewTimer;
    QList<ModelCard *> m_cards;
    ModelCard *m_selectedCard;
    QString m_pullJobId;
    QString m_downloadingId;
    QString m_lastSearch;
};
