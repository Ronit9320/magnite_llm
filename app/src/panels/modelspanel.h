#pragma once

#include "panels/basepanel.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>

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
// Phase 5.5.3: the old QTableWidget is a scrollable grid of model cards.
class ModelsPanel : public BasePanel {
    Q_OBJECT

public:
    explicit ModelsPanel(QWidget *parent = nullptr);

    void setBackendClient(BackendClient *client);
    void useLibraryWorkspace();

    QString settingsJson() const override;
    void applySettingsJson(const QString &json) override;

signals:
    void modelsChanged();
    void modelImported(const QString &name);
    void modelRemoved(const QString &id);
    void downloadFinished(bool success);
    void loadRequested(const QString &id);
    void workspaceRequested(int panel);
    void hubRequested();

public slots:
    void refresh();
    void importModel();

private slots:
    void onSearch();
    void onViewChanged(int index);
    void onDownload();
    void onDeleteModel();
    void onPullPoll();
    void onLoadMore();
    void onCancelDownload();

private:
    void setupUi();
    void clearCards();
    // tone: "" neutral | "ok" | "error" | "warn" — drives #statusLabel state.
    void setStatus(const QString &text, const QString &tone = QString());
    void switchToLocalView();
    void showSearchResults(const QJsonArray &results);
    void appendSearchResults(const QJsonArray &results);
    void requestSearchPage(int offset);
    void updateLoadMore();
    void reportHubStatus();
    void showLocalModels(const QJsonArray &models);
    void addCards(const QJsonArray &items, bool isHub, bool append = false);
    void startPullPolling(const QString &jobId);
    void onCardClicked(ModelCard *card);
    void updateActionButtons();
    void resetDownload();
    void showDownloadProgress(const QJsonObject &job);
    void sendCancel();
    void requestSizes(const QJsonArray &results);
    void confirmAndDelete(const QJsonObject &model, int dependentAdapters);
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
    QPushButton *m_loadMoreButton = nullptr;
    QPushButton *m_cancelButton = nullptr;
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
    bool m_searching = false;
    bool m_libraryOnly = false;
    // Cancel was pressed before the start request returned a job id, or the cancel
    // request is in flight; either way, do not send another.
    bool m_cancelPending = false;
    bool m_cancelling = false;
    // Said once after the next library refresh (e.g. "Deleted ..."), so the refresh does not hide it.
    QString m_afterRefreshNote;
    bool m_deleting = false;   // a delete is in flight: ignore further delete requests
    // Why the last cancel failed; shown with the progress until the user retries.
    QString m_cancelError;
    // Hub paging: results received so far (the next page's offset), whether the
    // last page was full, and the ids already shown (the Hub can reorder between pages).
    int m_hubOffset = 0;
    bool m_hasMoreResults = false;
    QSet<QString> m_hubIds;
};
