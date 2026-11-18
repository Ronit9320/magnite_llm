#pragma once

#include <QHash>
#include <QJsonObject>
#include <QWidget>
#include <optional>

class BlueprintCanvas;
class QTreeWidget;
class BlueprintPreview;
class Button;
class ComboBox;
class QDialog;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QScrollArea;
class QStackedWidget;
class TabWidget;
class QVBoxLayout;

class BlueprintPanel : public QWidget {
    Q_OBJECT
public:
    explicit BlueprintPanel(BlueprintPreview *preview, QWidget *parent = nullptr);
    void openRun(const QString &id);
    void showLibrary();

signals:
    void chatRequested();
    void conversationRequested(const QString &id);

protected:
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateCanvasFooter();
    void showRecentRuns();
    void refreshLibrary();
    void openBlueprint(const QString &id);
    void showBlueprint(const QJsonObject &graph);
    void showRun(const QString &id, const QJsonObject &run);
    void setupCreatePage();
    void showCreatePage();
    void createBlueprint();
    void createTriageExample();
    void editDetails();
    void deleteBlueprint(const QString &id);
    void duplicate(const QJsonObject &graph);
    bool saveGraph(QJsonObject graph, bool revision = true);
    void setupEditor();
    void refreshEditor(bool fit = false);
    void refreshInspector();
    void refreshNodeTree();
    void openNodePicker(std::optional<QPointF> position = {});
    void configureNode(const QString &id);
    void saveNodeTemplate(const QString &id);
    void addNode(const QJsonObject &node);
    void removeNode(const QString &id);
    void showNodeMenu(const QString &id, const QPoint &screenPosition, const QPointF &graphPosition);
    void renameNode(const QString &id);
    void connectPorts(const QString &inputNode = {}, const QString &inputPort = {});
    bool addConnection(const QJsonObject &edge, QWidget *parent);
    void editBinding();
    void editLiveBinding();
    void saveTrainingRecipe();
    void liveTraining();
    void editDataset();
    void publish();
    void reloadRevision();
    void editSchema(const QString &side, const QString &port);
    void changeNode(const QJsonObject &node, bool reconcilePorts = false);
    void toggleFullscreen();
    void leaveFullscreen();
    void refreshCanvasActions();
    void setupRun();
    void showTest(const QString &node = {});
    void startRun();
    void refreshRunOptions();
    void refreshRun();
    void refreshHistory();

    BlueprintPreview *m_preview;
    QJsonObject m_graph;
    QString m_selected;
    QString m_outputNode;
    QString m_outputPort;
    QString m_runId;
    QString m_traceId;
    QString m_traceStatus;
    QJsonObject m_baseline;
    bool m_comparison = false;
    bool m_dark;
    QHash<QString, bool> m_expanded;

    QStackedWidget *m_pages;
    QWidget *m_library;
    QWidget *m_workspace;
    QWidget *m_createPage;
    QLineEdit *m_newName;
    QPlainTextEdit *m_newDescription;
    QLabel *m_createError;
    QVBoxLayout *m_libraryBody;
    QLabel *m_title;
    QLabel *m_revision;
    QLabel *m_description;
    QLabel *m_service;
    Button *m_publish;
    Button *m_reload;
    Button *m_openInConversation;
    Button *m_backToChat;
    TabWidget *m_tabs;
    QVBoxLayout *m_canvasHome;
    QWidget *m_canvasPanel;
    QTreeWidget *m_nodeTree;
    QStackedWidget *m_canvasPages;
    BlueprintCanvas *m_canvas;
    Button *m_connect;
    Button *m_addNode;
    Button *m_emptyAdd;
    Button *m_fullscreen;
    QLabel *m_zoom;
    QLabel *m_gestureHint;
    QWidget *m_zoomControls;
    QLabel *m_canvasCount;
    Button *m_validation;
    QDialog *m_fullscreenDialog = nullptr;
    QVBoxLayout *m_inspector;

    QScrollArea *m_runSetup;
    ComboBox *m_target;
    ComboBox *m_sample;
    QWidget *m_sampleField;
    QWidget *m_scenarioField;
    ComboBox *m_scenario;
    QPlainTextEdit *m_input;
    QLabel *m_fixtureNote;
    QLabel *m_runError;
    Button *m_start;
    bool m_starting = false;
    quint64 m_navigation = 0;
    QScrollArea *m_traceScroll;
    QVBoxLayout *m_trace;
    QVBoxLayout *m_history;
};

class ConversationRunPanel : public QWidget {
    Q_OBJECT
public:
    enum class Mode { Tool, Blueprint };
    explicit ConversationRunPanel(BlueprintPreview *preview, Mode mode, QWidget *parent = nullptr);
    void activate(const QString &blueprint = {});
    void modelChanged(bool changing);
    bool hasActiveTask() const;

signals:
    void runRequested(const QString &id);
    void libraryRequested();
    void activityChanged();

protected:
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refreshPicker();
    void send();
    void updateActions();
    void rebuildTranscript();
    void refreshComposer();
    void openTaskHistory();
    BlueprintPreview *m_preview;
    const Mode m_mode;
    ComboBox *m_blueprint = nullptr;
    QPlainTextEdit *m_input;
    QScrollArea *m_scroll;
    QVBoxLayout *m_messages;
    QLabel *m_error;
    QLabel *m_inputHint;
    Button *m_send;
    Button *m_stop = nullptr;
    Button *m_newTask = nullptr;
    QJsonObject m_chatGraph;
    QString m_preferredBlueprint;
    QStringList m_taskRuns;
    quint64 m_selection = 0;
    bool m_starting = false;
    bool m_readyToSend = false;
    bool m_modelChanging = false;
    bool m_connected = false;
    bool m_dark;
};
