#include "testing/blueprint_tests.h"

#include "backend_client.h"
#include "backend_manager.h"
#include "panels/blueprintpanel.h"
#include "panels/blueprintpreview.h"
#include "theme.h"
#include "widgets/blueprintcanvas.h"

#include <QApplication>
#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QLabel>
#include <QLineEdit>
#include <QInputDialog>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTimer>
#include <QTemporaryDir>
#include <QWheelEvent>
#include <cstdio>
#include <memory>

namespace {
void pump(int ms = 15) {
    QEventLoop loop;
    QTimer::singleShot(ms, &loop, &QEventLoop::quit);
    loop.exec();
}

template<class Widget>
Widget *visibleNamed(QWidget &parent, const QString &name) {
    for (auto *widget : parent.findChildren<Widget *>())
        if (widget->isVisible() && widget->accessibleName() == name) return widget;
    return nullptr;
}

QPushButton *visibleButton(QWidget &parent, const QString &text) {
    for (auto *button : parent.findChildren<QPushButton *>())
        if (button->isVisible() && button->text() == text) return button;
    return nullptr;
}

bool visibleText(QWidget &parent, const QString &fragment) {
    for (const auto *label : parent.findChildren<QLabel *>())
        if (label->isVisible() && label->text().contains(fragment)) return true;
    return false;
}

void drag(QWidget *widget, const QPointF &from, const QPointF &to, int holdMs = 0) {
    QMouseEvent press(QEvent::MouseButtonPress, from, widget->mapToGlobal(from.toPoint()),
        Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent move(QEvent::MouseMove, to, widget->mapToGlobal(to.toPoint()),
        Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
    QMouseEvent release(QEvent::MouseButtonRelease, to, widget->mapToGlobal(to.toPoint()),
        Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
    QCoreApplication::sendEvent(widget, &press);
    QCoreApplication::sendEvent(widget, &move);
    if (holdMs) pump(holdMs);
    QCoreApplication::sendEvent(widget, &release);
}

void confirmDelete(QWidget *host) {
    QTimer::singleShot(0, host, [] {
        if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
            if (auto *submit = visibleButton(*dialog, "Delete")) submit->click();
            else dialog->reject();
        }
    });
}

bool waitForState(BlueprintPreview &preview, const QString &id, const QString &status, int timeout = 5000) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout) {
        if (preview.run(id)["status"].toString() == status) {
            pump();  // Let widgets added by runChanged finish entering the layout.
            return true;
        }
        pump(35);
    }
    return false;
}
}

int runBlueprintChecks(const QString &captures) {
    int failures = 0;
    auto check = [&failures](bool condition, const char *message) {
        failures += !condition;
        fprintf(stderr, "[blueprints] %s %s\n", condition ? "ok:" : "FAIL:", message);
    };
    auto capture = [&](QWidget &widget, const QString &name) {
        if (captures.isEmpty()) return;
        QDir().mkpath(captures);
        check(widget.grab().save(QDir(captures).filePath(name + ".png")), "saved reference capture");
    };
    const bool original = Theme::isDark();
    Theme::setDark(true);
    BlueprintPreview preview(nullptr, false);
    BlueprintPanel panel(&preview);
    panel.resize(1120, 720);
    panel.show();
    pump();

    auto *create = visibleButton(panel, "Create new");
    check(create && !panel.findChild<BlueprintCanvas *>()->isVisible(), "library starts with Create new and no open graph");
    capture(panel, "blueprints-library-dark");
    if (create) {
        create->click();
        pump();
        auto *name = visibleNamed<QLineEdit>(panel, "Name");
        auto *submit = visibleButton(panel, "Create blueprint");
        check(name && submit && !submit->isEnabled() && !panel.findChild<BlueprintCanvas *>()->isVisible(),
              "Create new opens an in-workspace form and requires a name");
        if (name) name->setText("Blank workflow");
        capture(panel, "blueprints-create-dark");
        if (submit) submit->click();
        pump();
    }
    check(preview.saved().size() == 1 && preview.saved().first().toObject()["nodes"].toArray().isEmpty()
              && visibleText(panel, "Start with a node"),
          "naming a blueprint opens an empty canvas and saves it");

    const auto blankId = preview.saved().isEmpty() ? QString{} : preview.saved().first().toObject()["id"].toString();
    auto *canvas = panel.findChild<BlueprintCanvas *>();
    if (auto *add = visibleButton(panel, "Add node")) add->click();
    pump();
    check(visibleNamed<QWidget>(panel, "Add node panel"), "Add node opens a docked palette");
    capture(panel, "blueprints-palette-dark");
    panel.resize(782, 628);
    pump();
    check(panel.width() == 782 && visibleNamed<QPushButton>(panel, "Add Input")
              && visibleNamed<QPushButton>(panel, "Add Dataset") && visibleNamed<QPushButton>(panel, "Add Transform"),
          "the compact palette lists general node kinds");
    if (auto *addInput = visibleNamed<QPushButton>(panel, "Add Input")) {
        QTimer::singleShot(0, &panel, [&] {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                if (auto *port = visibleNamed<QLineEdit>(*dialog, "Output port name")) port->setText("Payload");
                if (auto *type = visibleNamed<QComboBox>(*dialog, "Output port type"))
                    type->setCurrentIndex(type->findData("object"));
                capture(*dialog, "blueprints-node-config-dark");
                if (auto *submit = visibleButton(*dialog, "Create node")) submit->click();
                else dialog->reject();
            }
        });
        addInput->click();
    }
    if (auto *close = visibleNamed<QPushButton>(panel, "Close node palette")) close->click();
    pump();
    check(canvas->isVisible() && preview.blueprint(blankId)["nodes"].toArray().size() == 1,
          "choosing a palette item inserts and selects a node");
    const auto initialNodes = preview.blueprint(blankId)["nodes"].toArray();
    QString firstNodeId;
    if (canvas->isVisible() && !initialNodes.isEmpty()) {
        const auto node = initialNodes.first().toObject();
        firstNodeId = node["id"].toString();
        const auto port = node["out"].toArray().first().toObject();
        check(node["name"].toString() == "Input" && port["name"].toString() == "Payload"
                  && port["type"].toString() == "object",
              "a generic Input accepts a named and typed output without requiring a node name");
        const QPoint center = canvas->rect().center();
        bool nodeMenu = false;
        QTimer::singleShot(0, &panel, [&] {
            if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
                QStringList options;
                for (const auto *action : menu->actions()) options << action->text();
                nodeMenu = options.contains("Configure node…") && options.contains("Rename…")
                    && options.contains("Save to node library…") && options.contains("Delete node…");
                menu->close();
            }
        });
        QContextMenuEvent context(QContextMenuEvent::Mouse, center, canvas->mapToGlobal(center));
        QCoreApplication::sendEvent(canvas, &context);
        check(nodeMenu, "right-clicking a node offers configuration, rename, reuse, and deletion");
        drag(canvas, center, QPointF(-120, -120), 80);
        pump();
        const auto moved = preview.blueprint(blankId)["nodes"].toArray().first().toObject();
        check(moved["x"].toInt() < 0 && moved["y"].toInt() < 0,
              "left-dragging a node past the edge saves negative coordinates");
        if (auto *save = visibleNamed<QPushButton>(panel, "Save to node library")) {
            QTimer::singleShot(0, &panel, [] {
                if (auto *dialog = qobject_cast<QInputDialog *>(QApplication::activeModalWidget())) {
                    dialog->setTextValue("Object input");
                    dialog->accept();
                }
            });
            save->click();
            pump();
        }
        check(preview.nodeLibrary().size() == 1 && visibleNamed<QPushButton>(panel, "Add saved node Object input"),
              "saving a configured node adds a reusable copy to the palette");
        if (auto *remove = visibleNamed<QPushButton>(panel, "Delete node")) {
            confirmDelete(&panel);
            remove->click();
            pump();
        }
        check(preview.blueprint(blankId)["nodes"].toArray().isEmpty(), "deleting the final node returns to the empty canvas");
    }

    if (auto *library = visibleNamed<QPushButton>(panel, "Blueprint library")) library->click();
    pump();
    if (auto *newGraph = visibleButton(panel, "Create new")) newGraph->click();
    pump();
    if (auto *name = visibleNamed<QLineEdit>(panel, "Name")) name->setText("Reuse test");
    if (auto *submit = visibleButton(panel, "Create blueprint")) submit->click();
    pump();
    if (auto *add = visibleButton(panel, "Add node")) add->click();
    pump();
    if (auto *savedNode = visibleNamed<QPushButton>(panel, "Add saved node Object input")) savedNode->click();
    pump();
    const auto savedGraphs = preview.saved();
    const auto reusedGraph = savedGraphs.isEmpty() ? QJsonObject{} : savedGraphs.first().toObject();
    const auto reusedNodes = reusedGraph["nodes"].toArray();
    if (!reusedNodes.isEmpty()) {
        const auto reused = reusedNodes.first().toObject();
        const auto reusedPort = reused["out"].toArray().first().toObject();
        check(reusedGraph["id"].toString() != blankId && reused["id"].toString() != firstNodeId
                && reusedPort["name"].toString() == "Payload" && reusedPort["type"].toString() == "object",
              "a saved node inserted into another blueprint gets a new instance and keeps its ports");
    } else check(false, "a saved node can be inserted into another blueprint");
    if (reusedGraph["id"].toString() != blankId) preview.deleteBlueprint(reusedGraph["id"].toString());

    auto dataset = preview.makeNode("dataset-rows", "dataset", 0, 0);
    dataset["setting"] = R"([{"product": "A"}, {"product": "B"}, {"product": "C"}])";
    const auto counter = preview.makeNode("count", "counter", 200, 0);
    auto result = preview.makeNode("output", "result", 400, 0);
    const QJsonObject countPort{{"id", "value"}, {"name", "Total products"}, {"type", "integer"},
        {"schema", QJsonObject{{"type", "integer"}}}};
    result["in"] = QJsonArray{countPort};
    result["out"] = QJsonArray{countPort};
    const QJsonObject dataGraph{{"id", "dataset-count"}, {"name", "Count products"}, {"revision", 1},
        {"nodes", QJsonArray{dataset, counter, result}},
        {"edges", QJsonArray{
            QJsonObject{{"from", "dataset"}, {"out", "rows"}, {"to", "counter"}, {"in", "value"}},
            QJsonObject{{"from", "counter"}, {"out", "value"}, {"to", "result"}, {"in", "value"}}}}};
    QString datasetError;
    const auto datasetRun = preview.start(dataGraph, {}, {}, "normal", "Blueprints", &datasetError);
    const bool ranDataset = !datasetRun.isEmpty() && waitForState(preview, datasetRun, "passed");
    const auto dataSteps = preview.run(datasetRun)["steps"].toArray();
    check(ranDataset && !dataSteps.isEmpty() && dataSteps.last().toObject()["output"].toInt() == 3
              && dataSteps.last().toObject()["inputs"].toObject()["Total products"].toInt() == 3,
          "dataset rows flow through a count transform into a named output without an order sample");

    {
        BlueprintPreview writer(nullptr, true);
        const auto savedName = "Node persistence check " + BlueprintPreview::id();
        const bool saved = writer.saveNodeTemplate(savedName, dataset);
        BlueprintPreview reader(nullptr, true);
        const auto entries = reader.nodeLibrary();
        const auto stored = entries.isEmpty() ? QJsonObject{} : entries.first().toObject();
        check(saved && stored["name"].toString() == savedName
                  && stored["node"].toObject()["setting"] == dataset["setting"],
              "the node library reloads saved configuration from local settings");
        if (stored["name"].toString() == savedName) reader.removeNodeTemplate(stored["id"].toString());
    }

    panel.resize(1120, 720);
    auto showExample = [&] {
        if (auto *library = visibleNamed<QPushButton>(panel, "Blueprint library")) library->click();
        if (auto *example = visibleButton(panel, "Order intake example")) example->click();
        pump();
    };
    showExample();
    auto *adapter = visibleNamed<QComboBox>(panel, "Node adapter");
    check(canvas->isVisible() && visibleText(panel, "Node inspector") && adapter &&
              adapter->currentText() == "order-selector-v2" && adapter->findData("extractor") < 0,
          "starter example opens the graph and selected-node inspector");
    capture(panel, "blueprints-canvas-dark");
    panel.resize(782, 628);
    pump();
    check(panel.width() == 782 && canvas->width() > 350, "canvas and inspector fit the compact desktop workspace");
    capture(panel, "blueprints-canvas-compact-dark");
    panel.resize(1120, 720);
    pump();

    if (auto *full = visibleNamed<QPushButton>(panel, "Fullscreen canvas")) full->click();
    pump();
    check(canvas->window() != &panel && canvas->window()->isFullScreen(), "canvas opens in a native fullscreen window");
    if (canvas->isVisible()) {
        const QPointF point = canvas->rect().center();
        int percent = 0;
        const auto observation = QObject::connect(canvas, &BlueprintCanvas::zoomChanged, &panel, [&](int value) { percent = value; });
        QWheelEvent wheel(point, canvas->mapToGlobal(point.toPoint()), {}, QPoint(0, 120),
            Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(canvas, &wheel);
        check(wheel.isAccepted() && percent > 100, "wheel zooms the fullscreen canvas");
        const auto before = canvas->grab().toImage();
        const QPointF background(27, 24);
        drag(canvas, background, background + QPointF(63, 29));
        check(before != canvas->grab().toImage() && preview.saved().size() == 1,
              "left-dragging the background moves the camera without editing node positions");
        QObject::disconnect(observation);
        if (auto *exit = visibleNamed<QPushButton>(*canvas->window(), "Exit fullscreen")) exit->click();
        pump();
    }
    check(canvas->window() == &panel && canvas->isVisible(), "leaving fullscreen restores the original canvas");

    const auto graph = preview.example();
    const QJsonObject badEdge{{"from", "input"}, {"out", "text"}, {"to", "lookup"}, {"in", "index"}};
    check(preview.connectionError(graph, badEdge).contains("Text cannot connect to Integer"),
          "editor rejects incompatible typed ports");
    if (auto *runButton = visibleButton(panel, "Run blueprint")) runButton->click();
    pump();
    auto *start = visibleButton(panel, "Start run");
    auto *scenario = visibleNamed<QComboBox>(panel, "Fixture scenario");
    check(start && scenario && start->mapTo(&panel, QPoint()).x() > panel.width() / 2, "run setup sits to the right of the run trace");
    const auto graphId = graph["id"].toString();
    if (start) {
        start->click();
        const auto runId = preview.runs(graphId).first().toObject()["id"].toString();
        check(waitForState(preview, runId, "passed") &&
              preview.run(runId)["steps"].toArray().last().toObject()["output"].toObject()["total"].toInt() == 84,
              "full run uses the active clause and prices the order at $84");
        if (auto *compare = visibleButton(panel, "Compare baseline")) compare->click();
        pump();
        check(visibleText(panel, "$53.00"), "baseline comparison uses the same sample");
        if (scenario) scenario->setCurrentIndex(scenario->findData("failure"));
        start->click();
        const auto failureId = preview.runs(graphId).first().toObject()["id"].toString();
        check(waitForState(preview, failureId, "failed") && visibleText(panel, "output.quantity must be an integer")
                  && visibleNamed<QPushButton>(panel, "Failed · Extract fields"),
              "failed step shows an inline validation error and accessible status");
        capture(panel, "blueprints-run-dark");
        Theme::setDark(false);
        pump();
        capture(panel, "blueprints-run-light");
    }

    BlueprintChatPanel chat(&preview);
    chat.resize(900, 650);
    chat.show();
    chat.setMode(2);
    auto *message = visibleNamed<QPlainTextEdit>(chat, "Blueprint or tool message");
    if (message) message->setPlainText("Show the Python version.");
    if (auto *send = visibleButton(chat, "Send ↑")) send->click();
    const auto chats = preview.chatRuns();
    const auto toolId = chats.isEmpty() ? QString{} : chats.last().toObject()["id"].toString();
    check(!toolId.isEmpty() && waitForState(preview, toolId, "waiting") && visibleText(chat, "--version"),
          "tool chat pauses at an exact command fixture");
    panel.openRun(toolId);
    pump();
    check(visibleText(panel, "Working directory") && visibleButton(panel, "Back to chat"),
          "the exact chat run shares the approval view in Blueprints");
    if (auto *deny = visibleButton(panel, "Deny")) deny->click();
    pump();
    check(preview.run(toolId)["status"].toString() == "denied" && visibleText(chat, "The action was denied"),
          "denial updates the shared run and the chat result");
    if (auto *library = visibleNamed<QPushButton>(panel, "Blueprint library")) library->click();
    pump();
    if (auto *remove = visibleNamed<QPushButton>(panel, "Delete blueprint")) {
        confirmDelete(&panel);
        remove->click();
        pump();
    }
    check(preview.blueprint(blankId).isEmpty() && visibleButton(panel, "Create new"),
          "deleting a blueprint removes it from the library");
    Theme::setDark(original);
    fprintf(stderr, "[blueprints] %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures;
}

int runLiveBlueprintChecks(const QString &captures) {
    int failures = 0;
    auto check = [&failures](bool condition, const char *message) {
        failures += !condition;
        fprintf(stderr, "[blueprints-live] %s %s\n", condition ? "ok:" : "FAIL:", message);
        return condition;
    };
    auto capture = [&](QWidget &widget, const QString &name) {
        if (captures.isEmpty()) return;
        QDir().mkpath(captures);
        check(widget.grab().save(QDir(captures).filePath(name + ".png")), "saved live capture");
    };
    const auto until = [](const auto &condition, int timeout = 6000) {
        QElapsedTimer timer;
        timer.start();
        while (!condition() && timer.elapsed() < timeout) pump(30);
        return condition();
    };

    QTemporaryDir home;
    if (!check(home.isValid(), "created a clean local data folder")) return failures;
    qputenv("MAGNITE_HOME", home.path().toUtf8());
    BackendManager manager;
    manager.setAutoRestart(false);
    BackendClient client;
    QObject::connect(&manager, &BackendManager::started, &client, [&] {
        client.setAuthToken(manager.apiToken());
        client.setBaseUrl(manager.baseUrl());
    });
    BlueprintPreview session(client, nullptr);
    BlueprintPanel panel(&session);
    BlueprintChatPanel chat(&session);
    QObject::connect(&chat, &BlueprintChatPanel::runRequested, &panel, &BlueprintPanel::openRun);
    Theme::setDark(true);
    panel.resize(1000, 680);
    panel.show();
    manager.start();
    if (!check(until([&] { return client.isReady() && !session.orderSamples().isEmpty(); }, 35000),
               "native session is connected to the authenticated backend")) {
        fprintf(stderr, "%s\n", qUtf8Printable(manager.recentLogs()));
        return failures;
    }
    capture(panel, "live-library-dark");
    // Seed an editor draft with a deterministic Tool node. Run admission and
    // decisions still go through the real broker; denial executes no command.
    const auto graphId = BlueprintPreview::id();
    auto input = session.makeNode("input", "request", 0, 0);
    auto port = input["out"].toArray().first().toObject();
    port["schema"] = QJsonObject{{"type", "object"}};
    port["type"] = "object";
    input["out"] = QJsonArray{port};
    const auto tool = session.makeNode("approval", "approve", 231, 0);
    auto output = session.makeNode("output", "result", 462, 0);
    for (const auto &side : {"in", "out"}) {
        auto value = output[side].toArray().first().toObject();
        value["type"] = "object";
        value["schema"] = QJsonObject{{"type", "object"}};
        output[side] = QJsonArray{value};
    }
    const QJsonObject draft{{"id", graphId}, {"name", "Check an action"}, {"dirty", true}, {"revision", 0},
        {"nodes", QJsonArray{input, tool, output}}, {"edges", QJsonArray{
            QJsonObject{{"from", "request"}, {"out", "value"}, {"to", "approve"}, {"in", "value"}},
            QJsonObject{{"from", "approve"}, {"out", "receipt"}, {"to", "result"}, {"in", "value"}}}}};
    check(session.saveBlueprint(draft) && session.published().isEmpty(), "draft is visible only in the editor");
    pump();
    if (auto *open = visibleButton(panel, "Open")) open->click();
    pump();
    check(panel.findChild<BlueprintCanvas *>()->isVisible() && visibleText(panel, "Draft"),
          "canvas distinguishes an unpublished draft");
    capture(panel, "live-canvas-dark");
    QSettings().setValue("appearance/studio3ReducedMotion", true);
    panel.resize(740, 620);
    Theme::setDark(false);
    pump();
    check(panel.width() == 740 && panel.findChild<BlueprintCanvas *>()->isVisible(),
          "compact light canvas fits with reduced motion enabled");
    capture(panel, "live-canvas-compact-light");

    if (auto *test = visibleButton(panel, "Test this node")) test->click();
    pump();
    if (auto *message = visibleNamed<QPlainTextEdit>(panel, "Run input"))
        message->setPlainText(R"({"value":{"message":"isolated test"}})");
    if (auto *start = visibleButton(panel, "Test node")) start->click();
    check(until([&] { return visibleText(panel, "isolated test"); }), "isolated draft node returns the supplied typed input");

    if (auto *save = visibleButton(panel, "Save revision")) save->click();
    if (!check(until([&] { return !session.published().isEmpty() && visibleText(panel, "Revision 1"); }),
               "saving publishes a backend revision from the native editor")) return failures;
    chat.resize(740, 660);
    chat.show();
    chat.setMode(2);
    auto *send = visibleButton(chat, "Send ↑");
    check(until([&] { return visibleText(chat, "Switch to Chat to load"); }) && send && !send->isEnabled(),
          "Tool mode explains unavailability and disables sending without a loaded model");
    chat.setMode(1);
    auto *picker = visibleNamed<QComboBox>(chat, "Saved blueprint");
    if (picker) picker->setCurrentIndex(picker->findData(graphId));
    check(picker && picker->currentText().contains("Rev 1") && until([&] { return send && send->isEnabled(); }),
          "chat picker loads the saved revision and collects typed input");
    // Dirty edits to the editor must not change what runs from chat.
    auto changed = draft;
    changed["revision"] = 1;
    changed["name"] = "Unpublished edits";
    session.saveBlueprint(changed);
    const auto openRequest = QString::fromUtf8(R"({"request":{"action":"open","target":"https://example.com/blueprint-check"}})");
    if (auto *message = visibleNamed<QPlainTextEdit>(chat, "Blueprint or tool message")) message->setPlainText(openRequest);
    if (until([&] { return send && send->isEnabled(); })) {
        send->setFocus();
        QKeyEvent press(QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier);
        QKeyEvent release(QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier);
        QCoreApplication::sendEvent(send, &press);
        QCoreApplication::sendEvent(send, &release);
    }
    if (!check(until([&] { return visibleButton(chat, "Deny") && visibleText(chat, "Check an action"); }),
               "keyboard send runs the saved graph and pauses at the exact approval")) return failures;
    const auto runId = session.chatRuns().last().toObject()["id"].toString();
    const auto record = session.run(runId);
    check(record["revision"] == 1 && record["blueprintId"] == graphId &&
          visibleText(chat, "https://example.com/blueprint-check") && !visibleText(chat, "\"messages\": null"),
          "chat shows the persisted revision, submitted inputs, and exact target");
    capture(chat, "live-chat-approval-light");

    if (auto *open = visibleButton(chat, "Open full run")) open->click();
    pump();
    check(visibleText(panel, runId) && visibleButton(panel, "Deny"), "chat links to the same pending run in Blueprints");
    client.abortAll();
    client.setBaseUrl({});
    auto *offlineDeny = visibleButton(panel, "Deny");
    check(offlineDeny && !offlineDeny->isEnabled(), "pending decisions are unavailable while disconnected");
    client.setBaseUrl(manager.baseUrl());
    check(until([&] { return visibleButton(panel, "Deny") && visibleButton(panel, "Deny")->isEnabled(); }),
          "the pending decision is restored on reconnect");
    if (auto *deny = visibleButton(panel, "Deny")) deny->click();
    check(waitForState(session, runId, "failed") && visibleText(chat, "Denied") && visibleText(panel, "Tool action ended"),
          "denial updates the same durable run on both surfaces");
    Theme::setDark(true);
    pump();
    capture(panel, "live-run-failure-dark");
    check(session.run(runId)["decisions"].toArray().first().toObject()["status"] == "denied",
          "the persisted approval audit records the denial");

    const auto python = QStandardPaths::findExecutable("python3");
    if (!python.isEmpty()) {
        // The only command the check can approve prints Python's version.
        if (auto *target = visibleNamed<QComboBox>(panel, "Run target")) target->setCurrentIndex(0);
        if (auto *message = visibleNamed<QPlainTextEdit>(panel, "Run input"))
            message->setPlainText(BlueprintPreview::jsonText(QJsonObject{{"request",
                QJsonObject{{"action", "command"}, {"executable", python}, {"arguments", QJsonArray{"--version"}}}}}));
        if (auto *start = visibleButton(panel, "Start run")) start->click();
        if (check(until([&] { return visibleButton(panel, "Approve once") && visibleText(panel, "--version"); }) &&
                  visibleText(panel, "Working directory"), "Blueprints shows the executable, argument list, and working directory")) {
            const auto approvedId = session.runs(graphId).first().toObject()["id"].toString();
            capture(panel, "live-run-approval-dark");
            visibleButton(panel, "Approve once")->click();
            check(waitForState(session, approvedId, "passed") &&
                session.run(approvedId)["decisions"].toArray().first().toObject()["status"] == "completed",
                "Approve once completes the exact command through the broker");
        }
    }

    const auto testModel = qEnvironmentVariable("MAGNITE_BLUEPRINTS_LIVE_MODEL");
    if (!testModel.isEmpty()) {
        auto modelId = testModel;
        modelId.replace("/", "--");
        struct CheckpointCheck { QJsonObject info; QString error; bool ready = false; };
        auto checkpoint = std::make_shared<CheckpointCheck>();
        session.checkpoint("huggingface:" + modelId, [checkpoint](const auto &info, const auto &error) {
            *checkpoint = {info, error, true};
        });
        check(until([&] { return checkpoint->ready; }, 15000) && checkpoint->error.isEmpty()
              && checkpoint->info["context_tokens"].toInt() > 0 && checkpoint->info["reasoning"].isBool(),
              "checkpoint options report context and thinking support to the native binding picker");
        auto loadStatus = std::make_shared<int>(0);
        client.post("/inference/load", {{"model", testModel}, {"engine", "torch"}},
            [loadStatus](int status, const QJsonDocument &) { *loadStatus = status ? status : -1; });
        if (check(until([&] { return *loadStatus != 0; }, 30000) && *loadStatus == 200,
                  "loaded an offline local model for the native tool mode check")) {
            chat.setMode(2);
            if (auto *message = visibleNamed<QPlainTextEdit>(chat, "Blueprint or tool message"))
                message->setPlainText("Reply with the word ready. No external actions are needed.");
            if (until([&] { return send && send->isEnabled(); })) send->click();
            check(until([&] {
                const auto runs = session.chatRuns();
                const auto last = runs.last().toObject();
                return last["toolChat"].toBool() && last["status"] == "passed" && last["result"].isString();
            }, 30000), "native Tool mode sends an ad hoc task through Pi and displays its answer");
        }
    }

    client.abortAll();
    client.setBaseUrl({});
    manager.stop();
    fprintf(stderr, "[blueprints-live] %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures;
}
