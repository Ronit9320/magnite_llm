#pragma once

#include "panels/basepanel.h"

class QLineEdit;
class QPushButton;
class QTableWidget;
class QComboBox;
class QLabel;
class QTimer;
class BackendClient;

// Models panel: browse local models, search HuggingFace, download, and delete.
class ModelsPanel : public BasePanel {
    Q_OBJECT

public:
    explicit ModelsPanel(QWidget *parent = nullptr);

    void setBackendClient(BackendClient *client);

    QString settingsJson() const override;
    void applySettingsJson(const QString &json) override;

signals:
    void modelsChanged();

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
    void clearTable();
    void setStatus(const QString &text, const QString &color = QString());
    void switchToLocalView();
    void showSearchResults(const QJsonArray &results);
    void showLocalModels(const QJsonArray &models);
    void startPullPolling(const QString &jobId);
    bool isLocalView() const;
    QString selectedId() const;

    BackendClient *m_client;
    QLineEdit *m_searchEdit;
    QPushButton *m_searchButton;
    QPushButton *m_refreshButton;
    QPushButton *m_downloadButton;
    QPushButton *m_deleteButton;
    QComboBox *m_viewCombo;
    QTableWidget *m_table;
    QLabel *m_statusLabel;
    QTimer *m_pullTimer;
    QString m_pullJobId;
    QString m_lastSearch;
};