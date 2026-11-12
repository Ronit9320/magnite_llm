#pragma once

#include "panels/basepanel.h"

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QSet>
#include <functional>

class BackendClient;
class Button;
class ComboBox;
class FlowLayout;
class QCheckBox;
class QJsonDocument;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QStackedWidget;
class QTimer;
class QVBoxLayout;
class TabWidget;

// Native discovery workspace. Local model management lives in Library.
class ModelHubPanel : public BasePanel {
    Q_OBJECT
public:
    explicit ModelHubPanel(QWidget *parent = nullptr);
    void setBackendClient(BackendClient *client);
    QString settingsJson() const override;
    void applySettingsJson(const QString &json) override;

signals:
    void loadRequested(const QString &id);
    void modelsChanged();
    void deviceUpdated();
    void downloadFinished(bool success);

public slots:
    void refresh();
    void refreshLocalState();

protected:
    void showEvent(QShowEvent *event) override;

private:
    using Reply = std::function<void(int, const QJsonDocument &)>;
    void request(const QString &path, Reply reply);
    void setupDiscovery();
    void search(bool append = false);
    void renderCards();
    void updateFilterChoices();
    void resetFilters();
    void toggleComparison(const QString &repo, bool checked);
    void showComparison();
    void openDetails(const QString &repo);
    void renderDetails();
    void backToDiscovery();
    void updateEstimate();
    void refreshDevice();
    void updateDeviceUi();
    void showDeviceDetails();
    void startDownload();
    void cancelDownload();
    void sendCancel();
    void pollDownload();
    void finishDownload(const QString &message, bool success);
    void updateDownloadUi();
    void status(const QString &message, const QString &tone = {});

    BackendClient *m_client = nullptr;
    QStackedWidget *m_pages = nullptr;
    QWidget *m_discovery = nullptr;
    QWidget *m_details = nullptr;
    QVBoxLayout *m_detailLayout = nullptr;
    QLineEdit *m_search = nullptr;
    QPushButton *m_searchButton = nullptr;
    QPushButton *m_filterButton = nullptr;
    QWidget *m_filters = nullptr;
    ComboBox *m_task = nullptr;
    ComboBox *m_publisher = nullptr;
    ComboBox *m_format = nullptr;
    ComboBox *m_sort = nullptr;
    QCheckBox *m_localOnly = nullptr;
    QLabel *m_device = nullptr;
    QLabel *m_deviceMeta = nullptr;
    QProgressBar *m_deviceUsage = nullptr;
    QPushButton *m_deviceButton = nullptr;
    QLabel *m_detailDevice = nullptr;
    QLabel *m_estimateDetail = nullptr;
    QLabel *m_memoryStatus = nullptr;
    QWidget *m_memoryBar = nullptr;
    QString m_deviceError;
    QLabel *m_status = nullptr;
    QLabel *m_resultCount = nullptr;
    QLabel *m_empty = nullptr;
    QWidget *m_cardHost = nullptr;
    FlowLayout *m_flow = nullptr;
    QPushButton *m_loadMore = nullptr;
    QWidget *m_compareTray = nullptr;
    QLabel *m_compareCount = nullptr;
    QPushButton *m_compareButton = nullptr;
    QWidget *m_downloadStrip = nullptr;
    QLabel *m_downloadText = nullptr;
    QPushButton *m_cancelButton = nullptr;
    QProgressBar *m_progress = nullptr;
    QPushButton *m_detailAction = nullptr;
    QLabel *m_detailStatus = nullptr;
    ComboBox *m_weight = nullptr;
    ComboBox *m_context = nullptr;
    QLabel *m_estimate = nullptr;
    TabWidget *m_tabs = nullptr;
    QTimer *m_pullTimer = nullptr;

    QList<QJsonObject> m_models;
    QHash<QString, QJsonObject> m_metadata;
    QSet<QString> m_compared;
    QHash<QString, QJsonObject> m_comparisonData;
    QString m_renderedRepo;
    QJsonObject m_detailData;
    QString m_detailRepo;
    QString m_query;
    int m_offset = 0;
    quint64 m_searchGeneration = 0;
    quint64 m_detailGeneration = 0;
    bool m_searching = false;
    bool m_loaded = false;
    bool m_hasMore = false;
    bool m_detailLoading = false;
    QString m_detailError;
    QJsonObject m_telemetry;
    bool m_deviceLoading = false;
    QString m_pullRepo;
    QString m_pullJob;
    bool m_polling = false;
    bool m_cancelling = false;
    QJsonObject m_pullProgress;
};
