#pragma once

#include <QJsonArray>
#include <QJsonObject>
#include <QHash>
#include <QObject>
#include <QSet>
#include <functional>

class BackendClient;
class QComboBox;
class QJsonDocument;
class QTimer;

// Shared Blueprint state for the editor and chat. The fixture constructor is for design checks.
class BlueprintPreview : public QObject {
    Q_OBJECT
public:
    explicit BlueprintPreview(QObject *parent = nullptr, bool persist = true);
    BlueprintPreview(BackendClient &backend, QObject *parent);

    QJsonObject fixtures() const { return m_fixtures; }
    QJsonObject example() const;
    QJsonArray saved() const;
    QJsonArray nodeLibrary() const { return m_nodeLibrary; }
    bool saveNodeTemplate(const QString &name, QJsonObject node);
    bool removeNodeTemplate(const QString &id);
    QJsonObject operation(const QString &id) const;
    QString settingError(const QJsonObject &node) const;
    QString portLabel(const QJsonObject &port) const;
    QString nodeKindLabel(const QString &id) const;
    bool usesOrderSample(const QJsonObject &blueprint, const QString &target = {}) const;
    static QString operationId(const QJsonObject &node);
    QJsonObject blueprint(const QString &id) const;
    bool saveBlueprint(const QJsonObject &blueprint);
    bool deleteBlueprint(const QString &id);
    QJsonArray compatibleAdapters(const QString &model, const QString &kind) const;
    QJsonObject run(const QString &id) const;
    QJsonArray runs(const QString &blueprintId) const;
    QJsonArray chatRuns() const;
    QString start(const QJsonObject &blueprint, const QString &input, const QString &target,
                  const QString &scenario, const QString &source, QString *error);
    void decide(const QString &id, bool approved, const QJsonObject &displayed = {});
    void cancel(const QString &id);

    using Result = std::function<void(const QJsonObject &, const QString &error)>;
    bool live() const { return m_backend != nullptr; }
    bool connected() const;
    QJsonArray published() const { return m_published; }
    QJsonArray recentRuns() const;
    QJsonArray datasets() const { return m_datasets; }
    QString serviceError() const { return m_serviceError; }
    void refreshSaved();
    void refreshOptions();
    void checkpoint(const QString &model, Result done);
    void getBlueprint(const QString &id, bool publishedOnly, Result done, qint64 revision = 0);
    void publish(const QJsonObject &graph, Result done);
    void removeBlueprint(const QString &id, Result done);
    void validateLive(const QJsonObject &graph, Result done);
    void runLive(const QJsonObject &graph, const QJsonValue &input, const QString &target,
                 bool chat, Result done);
    void runToolTask(const QJsonArray &messages, Result done);
    void loadRun(const QString &id, Result done = {});
    void refreshHistory(const QString &id);
    void toolCapability(Result done);
    static QString modelLabel(const QString &reference);
    static void disableItem(QComboBox *combo, int index);
    void train(const QJsonObject &graph, const QString &node, const QJsonArray &examples,
               const QJsonObject &settings, Result done);
    void jobs(const QString &blueprint, const QString &node, Result done);
    void bindTraining(const QJsonObject &graph, const QString &node, const QString &job, Result done);
    static QJsonObject inputContracts(const QJsonObject &graph, const QString &target);
    QStringList issues(const QJsonObject &blueprint) const;
    QString connectionError(const QJsonObject &blueprint, const QJsonObject &edge) const;
    bool choosesType(const QJsonObject &node, const QString &side, const QString &portId) const;
    QJsonObject withPortType(const QJsonObject &node, const QString &side, const QString &portId, const QString &type) const;
    QJsonObject connectGraph(const QJsonObject &blueprint, const QJsonObject &edge, int *droppedEdges = nullptr,
                             QString *error = nullptr) const;
    QString dropError(const QJsonObject &blueprint, const QJsonObject &edge) const;
    QJsonObject makeNode(const QString &kind, const QString &id, int x, int y) const;
    static QString id();
    static QString statusLabel(const QString &status);
    static QString duration(qint64 ms);
    static QString jsonText(const QJsonValue &value);
    static QJsonValue parseJsonValue(const QString &text);
    static QJsonObject parseRule(const QString &text);
    static QString ruleText(const QJsonObject &rule);
    static QJsonObject parseSort(const QString &text);
    static bool supportedSchema(const QJsonObject &schema);

signals:
    void savedChanged();
    void nodeLibraryChanged();
    void runChanged(const QString &id);
    void optionsChanged();
    void serviceChanged();

private:
    BlueprintPreview(BackendClient *backend, QObject *parent, bool persist);
    using Response = std::function<void(int, const QJsonDocument &)>;
    void request(const QString &method, const QString &path, const QJsonObject &body, Response done);
    void fetchSaved(int offset, QJsonArray entries, quint64 request);
    void submitRun(const QString &path, const QJsonObject &body, Result done);
    void refreshRuns(const QString &query, bool openChat = false);
    void rememberRun(const QJsonObject &run);
    void connectRun(const QString &id);
    void decideLive(const QString &id, bool approved, const QJsonObject &displayed);
    void cancelLive(const QString &id);
    void reviseLocal(const QJsonObject &revision, QJsonObject editor, Result done);
    void tick();
    bool storeBlueprints(const QJsonArray &saved);
    bool storeNodeLibrary(const QJsonArray &nodes);
    void storeRuns();
    bool advance(QJsonObject &run, qint64 now) const;
    void updateRun(int index, const QJsonObject &run);

    QJsonObject m_fixtures;
    QJsonArray m_saved;
    QJsonArray m_nodeLibrary;
    QJsonArray m_runs;
    bool m_persist;
    QTimer *m_timer;
    BackendClient *m_backend;
    QByteArray m_draftsKey;
    QByteArray m_templatesKey;
    QJsonArray m_published;
    QJsonArray m_datasets;
    QString m_serviceError;
    quint64 m_session = 0;
    quint64 m_savedRequest = 0;
    QSet<QString> m_streams;
    QHash<QString, qint64> m_cursors;
    QHash<QString, QList<Result>> m_refreshingRuns;
    QSet<QString> m_dirtyRuns;
    QSet<QString> m_actions;
    QHash<QString, QString> m_actionErrors;
};
