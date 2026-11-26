#pragma once

#include "panels/blueprinttopbar.h"

#include <QHash>
#include <QJsonObject>
#include <QWidget>
#include "panels/blueprintlibrarydock.h"
#include <optional>

class BlueprintView;
class FloatingPanel;
class BlueprintDetails;
class BlueprintInspector;
class BlueprintQuickAdd;
class FloatingPanel;
class QTimer;
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
    void refreshBar();
    void showView(BlueprintTopBar::View view);
    // The bar floats over the canvas in Build (and fullscreen) and sits above the page otherwise.
    void placeBar();
    void checkValidation(int issue);
    void showRunMenu(const QPoint &globalPosition);
    // With a node selected, a canvas narrower than 240px between the panels closes the library.
    void applyCompactRule();
    void refreshInspector();
    void addEntry(const BlueprintLibraryDock::Entry &entry, std::optional<QPointF> at = {});
    void openQuickAdd(QWidget *host, const QPoint &position, const QPointF &graph,
                      const QString &fromNode = {}, const QString &outPort = {});
    QJsonObject entryNode(const BlueprintLibraryDock::Entry &entry, const QPointF &at) const;
    QString connectedInput(const BlueprintLibraryDock::Entry &entry, const QString &fromNode, const QString &outPort) const;
    void addConnectedEntry(const BlueprintLibraryDock::Entry &entry, const QPointF &at,
                           const QString &fromNode, const QString &outPort);
    void configureNode(const QString &id);
    void saveNodeTemplate(const QString &id);
    void addNode(const QJsonObject &node);
    void removeNode(const QString &id);
    void showNodeMenu(const QString &id, const QPoint &screenPosition, const QPointF &graphPosition);
    void renameNode(const QString &id);
    void connectPorts(const QString &inputNode = {}, const QString &inputPort = {});
    bool addConnection(const QJsonObject &edge, QWidget *parent);
    bool dropConnection(const QJsonObject &edge);
    bool removeConnection(const QJsonObject &edge);
    bool moveConnection(const QJsonObject &oldEdge, const QJsonObject &newEdge);
    bool saveConnected(const QJsonObject &base, const QJsonObject &edge);
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
    QWidget *setupRun();
    void showTest(const QString &node = {});
    void startRun();
    void runFromCanvas();
    void showNodeDetails(const QString &id);
    void showEntryDetails(const BlueprintLibraryDock::Entry &entry, const QRect &anchor);
    void showCanvasRun(const QJsonObject &run);
    void showRunToast(const QJsonObject &run);
    void refreshRunOptions();
    void refreshRun();
    void refreshHistory();

    BlueprintPreview *m_preview;
    QJsonObject m_graph;
    QString m_selected;
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
    QLabel *m_service;
    BlueprintTopBar *m_bar;
    QVBoxLayout *m_barSlot;
    QStackedWidget *m_views;  // Build · Test cases · Run history, in BlueprintTopBar::View order
    bool m_canGoBack = false;  // the open run came from Conversation
    BlueprintLibraryDock *m_libraryDock;
    BlueprintQuickAdd *m_quickAdd = nullptr;
    QString m_canvasRunId;
    QString m_toastedRun;
    bool m_runOnCanvas = false;
    bool m_runAfterPublish = false;
    FloatingPanel *m_toast = nullptr;
    BlueprintDetails *m_details = nullptr;
    QLabel *m_toastText = nullptr;
    QTimer *m_toastTimer = nullptr;
    FloatingPanel *m_libraryPanel;
    QStackedWidget *m_canvasPages;
    BlueprintView *m_canvas;
    QWidget *m_emptyCanvas;
    Button *m_emptyAdd;
    QDialog *m_fullscreenDialog = nullptr;
    FloatingPanel *m_inspectorPanel;
    QVBoxLayout *m_inspector;
    BlueprintInspector *m_nodeInspector;

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
