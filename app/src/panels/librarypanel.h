#pragma once

#include "panels/basepanel.h"
#include <QJsonArray>
#include <QJsonObject>

class AdaptersPanel;
class ModelsPanel;
class Button;
class ChatStore;
class BackendClient;
class ProgressBar;
class ComboBox;
class QLabel;
class QLineEdit;
class QVBoxLayout;
class TabWidget;

class LibraryPanel : public BasePanel {
    Q_OBJECT
public:
    LibraryPanel(ModelsPanel *models, AdaptersPanel *adapters);
    void showAdapters();
    QStringList chatProjects(const QString &id) const;
    void setChatStore(ChatStore *store);
    void setBackendClient(BackendClient *client);
    void recordActivity(const QString &name, const QString &action, const QString &status,
                        const QString &detail = {});
signals:
    void organizationChanged();
    void openChatRequested(const QString &id);
    void retryDownload(const QString &repo);
private:
    void updateView();
    void filterItems();
    void renderCollections();
    void renderHistory();
    void createCollection();
    void deleteCollection(bool folder, const QString &name);
    void renderCollectionContents(QVBoxLayout *layout, bool folder);
    void addChat(bool folder);
    void addFiles(bool folder);
    void organize(const QString &key, const QString &name);
    void save();
    void refreshStorage();

    ModelsPanel *m_models;
    AdaptersPanel *m_adapters;
    TabWidget *m_tabs;
    Button *m_action;
    QLabel *m_storage;
    QLabel *m_diskUsage;
    ProgressBar *m_meter;
    BackendClient *m_client = nullptr;
    QLabel *m_scopeLabel;
    QLineEdit *m_search;
    Button *m_pinned;
    ComboBox *m_filter;
    ComboBox *m_sort;
    QLabel *m_results;
    QVBoxLayout *m_projects;
    QVBoxLayout *m_folders;
    QVBoxLayout *m_history;
    QJsonArray m_projectNames;
    QJsonArray m_folderNames;
    QJsonObject m_memberships;
    QJsonArray m_events;
    QJsonObject m_collectionInfo;
    ChatStore *m_chatStore = nullptr;
    QString m_openProject;
    QString m_openFolder;
    QString m_scope;
    bool m_folderScope = false;
};
