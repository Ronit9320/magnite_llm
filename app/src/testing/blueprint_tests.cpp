#include "testing/blueprint_tests.h"
#include "testing/blueprint_test_support.h"

#include "backend_client.h"
#include "backend_manager.h"
#include "panels/blueprintpanel.h"
#include "panels/blueprintpreview.h"
#include "panels/chatpanel.h"
#include "theme.h"
#include "widgets/blueprintscene.h"
#include "widgets/blueprintview.h"

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
#include <QScreen>
#include <QScrollArea>
#include <QSettings>
#include <QStandardPaths>
#include <QTabWidget>
#include <QTimer>
#include <QTemporaryDir>
#include <QTreeWidget>
#include <QWheelEvent>
#include <QtMath>
#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>

using namespace BlueprintTest;

namespace {

// The workflow node tree, shown or not: the compact rule may have closed the library it sits in.
QTreeWidget *nodeTree(QWidget &panel) {
    for (auto *tree : panel.findChildren<QTreeWidget *>())
        if (tree->accessibleName() == "Workflow nodes and connections") return tree;
    return nullptr;
}

// The texts of the top bar's ⋯ menu (the popup closes again).
QStringList moreActions(QWidget &panel) {
    QStringList texts;
    auto *more = visibleNamed<QPushButton>(panel, "More blueprint actions");
    if (!more) return texts;
    QTimer::singleShot(0, more, [&texts] {
        if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) {
            for (const auto *action : menu->actions())
                if (action->isVisible() && !action->isSeparator()) texts << action->text();
            menu->close();
        }
    });
    more->click();
    pump();
    return texts;
}

QString modelId(QString repo) {
    return "huggingface:" + repo.replace("/", "--");
}


bool runModelBlueprintAcceptance(BlueprintPreview &session, BlueprintPanel &panel, ChatPanel &chat,
    const QString &model, const QString &reasoningModel, const QString &captures) {
    const auto check = [](bool passed, const char *step) {
        fprintf(stderr, "[blueprints-model] %s %s\n", passed ? "ok:" : "FAIL:", step);
        return passed;
    };
    const auto publishedRevision = [&](const QString &id) {
        for (const auto &value : session.published()) {
            const auto entry = value.toObject();
            if (entry["id"] == id) return entry["revision"].toInt();
        }
        return 0;
    };
    const auto back = [&] {
        if (auto *button = visibleNamed<QPushButton>(panel, "Back to library")) button->click();
        pump();
    };
    back();
    if (auto *create = visibleButton(panel, "Create new")) create->click();
    pump();
    const QString name = "Local adapter acceptance";
    if (auto *title = visibleNamed<QLineEdit>(panel, "Name")) title->setText(name);
    if (auto *create = visibleButton(panel, "Create blueprint")) create->click();
    pump();
    QJsonObject draft;
    for (const auto &value : session.saved())
        if (value.toObject()["name"] == name) draft = value.toObject();
    if (!check(!draft["id"].toString().isEmpty() && draft["nodes"].toArray().isEmpty(),
               "the desktop creates a blank draft")) return false;
    const auto id = draft["id"].toString();
    auto answer = session.makeNode("generate", "answer", 230, 0);
    answer["prompt"] = "Answer the user with a brief factual answer.";
    answer["response"] = "text";
    answer["maxTokens"] = 24;
    draft["nodes"] = QJsonArray{session.makeNode("input", "question", 0, 0), answer,
                               session.makeNode("output", "result", 460, 0)};
    draft["edges"] = QJsonArray{edge("question", "value", "answer", "value"), edge("answer", "value", "result", "value")};
    if (!session.saveBlueprint(draft)) return false;
    const auto open = [&] {
        for (auto *button : panel.findChildren<QPushButton *>()) {
            if (button->isVisible() && button->text() == "Open" && visibleText(*button->parentWidget(), name)) {
                button->click();
                pump();
                return true;
            }
        }
        return false;
    };
    back();
    if (!check(open(), "the draft reopens from the native library")) return false;
    auto *tree = nodeTree(panel);
    if (!tree || tree->topLevelItemCount() != 3)
        return check(false, "the Model node appears in the workflow tree");
    tree->setCurrentItem(tree->topLevelItem(1));
    pump();

    // Enter the actual modal selector and wait for the checkpoint options request.
    QElapsedTimer dialogTime;
    QTimer driver;
    driver.setInterval(100);
    auto bindingDriver = QObject::connect(&driver, &QTimer::timeout, &panel, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) return;
        auto *checkpoints = visibleNamed<QComboBox>(*dialog, "Local checkpoint");
        if (dialogTime.elapsed() > 20000) {
            fprintf(stderr, "[blueprints-model] Checkpoint picker entries: %d\n", checkpoints ? checkpoints->count() : 0);
            for (const auto *error : dialog->findChildren<QLabel *>("blueprintError"))
                if (error->isVisible()) fprintf(stderr, "[blueprints-model] %s\n", qUtf8Printable(error->text()));
            if (!captures.isEmpty()) dialog->grab().save(QDir(captures).filePath("model-checkpoint-picker.png"));
            dialog->reject();
            return;
        }
        if (!checkpoints || checkpoints->findData(modelId(model)) < 0) return;
        checkpoints->setCurrentIndex(checkpoints->findData(modelId(model)));
        if (auto *save = visibleButton(*dialog, "Save changes"); save && save->isEnabled()) save->click();
    });
    dialogTime.start();
    driver.start();
    if (auto *configure = visibleButton(panel, "Configure…")) configure->click();
    else fprintf(stderr, "[blueprints-model] Configure button unavailable after selecting the model\n");
    driver.stop();
    QObject::disconnect(bindingDriver);
    if (!check(session.blueprint(id)["nodes"].toArray().at(1).toObject()["model"] == modelId(model),
               "the Model node binds the local checkpoint in the desktop picker")) return false;
    if (auto *save = visibleButton(panel, "Publish")) save->click();
    if (!check(waitFor([&] { return publishedRevision(id) == 1 && visibleText(panel, "Revision 1"); }),
               "the model graph publishes revision 1")) return false;

    const QString question = "What is the capital of the United Kingdom?";
    const QJsonArray examples{
        QJsonObject{{"inputs", QJsonObject{{"value", question}}}, {"outputs", QJsonObject{{"value", "London."}}}},
        QJsonObject{{"inputs", QJsonObject{{"value", "Name the capital of the United Kingdom."}}},
                    {"outputs", QJsonObject{{"value", "London."}}}}};
    bool submitted = false;
    auto jobDriver = QObject::connect(&driver, &QTimer::timeout, &panel, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) return;
        if (dialogTime.elapsed() > 120000) { dialog->reject(); return; }
        if (!submitted) {
            auto *editor = visibleNamed<QPlainTextEdit>(*dialog, "Typed examples (JSON array)");
            auto *queue = visibleButton(*dialog, "Queue training");
            if (editor && queue && queue->isEnabled()) {
                editor->setPlainText(BlueprintPreview::jsonText(examples));
                submitted = true;
                queue->click();
            }
            return;
        }
        auto *jobs = visibleNamed<QComboBox>(*dialog, "Training jobs for this node");
        if (!jobs) return;
        for (int i = 1; i < jobs->count(); ++i) {
            if (jobs->itemText(i).endsWith(" · error")) {
                fprintf(stderr, "[blueprints-model] %s\n", qUtf8Printable(jobs->itemData(i, Qt::ToolTipRole).toString()));
                dialog->reject();
                return;
            }
            if (!jobs->itemData(i, Qt::UserRole + 1).toBool()) continue;
            jobs->setCurrentIndex(i);
            if (auto *bind = visibleButton(*dialog, "Bind completed adapter"); bind && bind->isEnabled()) bind->click();
            return;
        }
    });
    dialogTime.restart();
    driver.start();
    if (auto *training = visibleButton(panel, "Training jobs…")) training->click();
    driver.stop();
    QObject::disconnect(jobDriver);
    const auto trained = session.blueprint(id);
    const auto adapter = trained["nodes"].toArray().at(1).toObject()["adapter"].toString();
    if (!check(submitted && publishedRevision(id) == 2 && !adapter.isEmpty(),
               "the desktop queues real training and binds the completed adapter as revision 2")) return false;

    const auto latest = [&] {
        const auto runs = session.runs(id);
        return runs.isEmpty() ? QJsonObject{} : runs.first().toObject();
    };
    const auto runInEditor = [&](const QString &buttonText) {
        if (auto *input = visibleNamed<QPlainTextEdit>(panel, "Run input")) input->setPlainText(question);
        const auto before = latest()["id"].toString();
        auto *start = visibleButton(panel, buttonText);
        if (!start || !start->isEnabled()) {
            fprintf(stderr, "[blueprints-model] Run action unavailable: %s\n", qUtf8Printable(buttonText));
            return false;
        }
        start->click();
        const bool finished = waitFor([&] {
            const auto run = latest();
            if (run["id"] != before && (run["status"] == "passed" || run["status"] == "failed")) return true;
            for (const auto *error : panel.findChildren<QLabel *>("blueprintError"))
                if (error->isVisible() && !error->text().isEmpty()) return true;
            return false;
        }, 35000);
        if (!finished || latest()["status"] != "passed") {
            for (const auto *error : panel.findChildren<QLabel *>("blueprintError"))
                if (error->isVisible()) fprintf(stderr, "[blueprints-model] %s\n", qUtf8Printable(error->text()));
            fprintf(stderr, "[blueprints-model] Latest run: %s\n", qUtf8Printable(BlueprintPreview::jsonText(latest())));
            return false;
        }
        return true;
    };
    pump(); // The completed-job dialog rebuilds the inspector before it closes.
    auto *test = visibleButton(panel, "Test this node");
    if (!check(test && test->isEnabled(), "the trained node offers a desktop test")) return false;
    test->click();
    pump();
    if (!check(runInEditor("Test node") && latest()["target"] == "answer",
               "the bound Model node passes its isolated desktop test")) return false;
    if (auto *target = visibleNamed<QComboBox>(panel, "Run target")) target->setCurrentIndex(0);
    if (!check(runInEditor("Start run") && latest()["revision"].toInt() == 2,
               "Blueprints runs the complete trained graph on revision 2")) return false;
    const auto editorRun = latest();
    if (!captures.isEmpty()) panel.grab().save(QDir(captures).filePath("trained-blueprint-run.png"));
    back();
    if (!check(open() && visibleText(panel, "Revision 2"), "the trained revision reopens from the native library")) return false;
    chat.showBlueprint(id);
    if (!check(waitFor([&] { return visibleNamed<QPlainTextEdit>(chat, "Workflow inputs"); }),
               "Conversation loads inputs from the saved trained revision")) return false;
    visibleNamed<QPlainTextEdit>(chat, "Workflow inputs")->setPlainText(question);
    const auto send = [&] { return visibleButton(chat, "Run workflow"); };
    if (waitFor([&] { return send() && send()->isEnabled(); })) send()->click();
    const bool chatFinished = waitFor([&] {
        const auto run = latest();
        return run["source"] == "Local chat" && (run["status"] == "passed" || run["status"] == "failed");
    }, 35000);
    if (!check(chatFinished && latest()["status"] == "passed" && latest()["revision"].toInt() == 2 &&
               latest()["result"] == editorRun["result"], "chat runs the same saved revision and returns the same result")) return false;
    if (!captures.isEmpty()) chat.grab().save(QDir(captures).filePath("trained-blueprint-chat.png"));

    auto linked = trained;
    auto linkedNodes = linked["nodes"].toArray();
    auto linkedModel = linkedNodes.at(1).toObject();
    linkedModel.remove("adapter");
    linkedNodes.replace(1, linkedModel);
    linkedNodes.append(session.makeNode("lora", "lora", 230, 180));
    auto linkedEdges = linked["edges"].toArray();
    linkedEdges.append(edge("lora", "adapter", "answer", "adapter"));
    linked["nodes"] = linkedNodes;
    linked["edges"] = linkedEdges;
    linked["dirty"] = true;
    if (!session.saveBlueprint(linked)) return false;
    back();
    if (!open()) return false;
    tree = nodeTree(panel);
    if (!check(tree && tree->topLevelItemCount() == 4 && tree->topLevelItem(3)->childCount() == 1,
               "the workflow tree shows the Adapter node and its outgoing link")) return false;
    tree->setCurrentItem(tree->topLevelItem(3));
    pump();
    const auto adapterDriver = QObject::connect(&driver, &QTimer::timeout, &panel, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) return;
        if (dialogTime.elapsed() > 20000) { dialog->reject(); return; }
        auto *adapters = visibleNamed<QComboBox>(*dialog, "Trained adapter");
        if (!adapters || adapters->findData(adapter) < 0) return;
        adapters->setCurrentIndex(adapters->findData(adapter));
        if (auto *save = visibleButton(*dialog, "Save changes")) save->click();
    });
    dialogTime.restart();
    driver.start();
    if (auto *configure = visibleButton(panel, "Configure…")) configure->click();
    driver.stop();
    QObject::disconnect(adapterDriver);
    if (!check(session.blueprint(id)["nodes"].toArray().at(3).toObject()["adapter"] == adapter,
               "the Adapter node can select the real trained weights")) return false;
    if (auto *save = visibleButton(panel, "Publish")) save->click();
    if (!check(waitFor([&] { return publishedRevision(id) == 3 && visibleText(panel, "Revision 3"); }),
               "the LoRA link persists on a new revision")) return false;
    pump();
    if (!captures.isEmpty()) panel.grab().save(QDir(captures).filePath("linked-adapter-graph.png"));
    tree->setCurrentItem(tree->topLevelItem(1));
    pump();
    if (auto *testLinked = visibleButton(panel, "Test this node")) testLinked->click();
    if (!check(runInEditor("Test node") && latest()["target"] == "answer",
               "an isolated checkpoint test resolves the linked LoRA adapter")) return false;
    if (auto *target = visibleNamed<QComboBox>(panel, "Run target")) target->setCurrentIndex(0);
    if (!check(runInEditor("Start run") && latest()["revision"].toInt() == 3 &&
               latest()["result"] == editorRun["result"], "local generation uses the linked adapter")) return false;

    if (reasoningModel.isEmpty()) return true;
    const QJsonObject integer{{"type", "integer"}};
    const QJsonObject resultSchema{{"type", "object"}, {"required", QJsonArray{"result"}},
        {"properties", QJsonObject{{"result", integer}}}, {"additionalProperties", false}};
    auto reason = session.makeNode("reason", "reason", 0, 0);
    reason["in"] = QJsonArray{QJsonObject{{"id", "index"}, {"type", "integer"}, {"schema", integer}}};
    reason["out"] = QJsonArray{QJsonObject{{"id", "answer"}, {"type", "object"}, {"schema", resultSchema}}};
    reason["prompt"] = "Multiply 17 by 23 and add the number provided by the user. "
        "After reasoning, output a JSON object with one key named \"result\" and an integer value.";
    reason["maxTokens"] = 1200;
    auto checkReasoning = [&](const QString &repo) {
        reason["model"] = modelId(repo);
        auto response = std::make_shared<QJsonObject>();
        const QJsonObject graph{{"id", BlueprintPreview::id()}, {"name", "Check typed reasoning"}, {"nodes", QJsonArray{reason}}};
        session.runLive(graph, QJsonObject{{"index", 2}}, "reason", false,
            [response](const auto &run, const auto &error) { *response = {{"run", run}, {"error", error}}; });
        waitFor([&] { return !response->isEmpty(); }, 20000);
        return *response;
    };
    if (!check(checkReasoning(model)["error"].toString().contains("explicit thinking"),
               "unsupported thinking reports the required checkpoint capability")) return false;
    const auto reasonRun = checkReasoning(reasoningModel)["run"].toObject()["id"].toString();
    return check(!reasonRun.isEmpty() && waitForState(session, reasonRun, "passed", 45000) &&
                 session.run(reasonRun)["result"].toObject()["reason"].toObject()["answer"].toObject()["result"].toInt() == 393,
                 "the second checkpoint produces the typed Reasoning answer 393");
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
        pump(200);  // let deferred layouts (the inspector's scroll area) settle before grabbing
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
    check(create && !panel.findChild<BlueprintView *>()->isVisible(), "library starts with Create new and no open graph");
    capture(panel, "blueprints-library-dark");
    if (create) {
        create->click();
        pump();
        auto *name = visibleNamed<QLineEdit>(panel, "Name");
        auto *submit = visibleButton(panel, "Create blueprint");
        check(name && submit && !submit->isEnabled() && !panel.findChild<BlueprintView *>()->isVisible(),
              "Create new opens an in-workspace form and requires a name");
        if (name) name->setText("Blank workflow");
        capture(panel, "blueprints-create-dark");
        if (submit) submit->click();
        pump();
    }
    check(preview.saved().size() == 1 && preview.saved().first().toObject()["nodes"].toArray().isEmpty()
              && visibleText(panel, "Start with a node"),
          "naming a blueprint opens an empty canvas and saves it");
    check(!visibleNamed<QPushButton>(panel, "Fullscreen canvas"),
          "an empty canvas offers no fullscreen button that would do nothing");

    const auto blankId = preview.saved().isEmpty() ? QString{} : preview.saved().first().toObject()["id"].toString();
    auto *canvas = panel.findChild<BlueprintView *>();
    if (auto *add = visibleButton(panel, "Add node")) add->click();
    pump();
    auto *picker = visibleNamed<QDialog>(panel, "Add node picker");
    check(picker, "Add node opens a separate themed dialog");
    if (picker) {
        const auto cards = picker->findChildren<QPushButton *>("blueprintNodeCard");
        check(cards.size() == 6 && !picker->findChild<QScrollArea *>() &&
              cards[0]->y() == cards[1]->y() && cards[0]->x() < cards[1]->x() && cards[3]->y() > cards[0]->y(),
              "the node picker lays out built-in cards across columns without scrolling");
        capture(*picker, "blueprints-palette-dark");
        Theme::setDark(false);
        pump();
        capture(*picker, "blueprints-palette-light");
        Theme::setDark(true);
        pump();
    }
    panel.resize(782, 628);
    pump();
    check(panel.width() == 782 && visibleNamed<QPushButton>(panel, "Add Input")
              && visibleNamed<QPushButton>(panel, "Add Dataset") && visibleNamed<QPushButton>(panel, "Add Transform"),
          "the node picker lists general node kinds in the compact workspace");
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
    pump();
    check(canvas->isVisible() && preview.blueprint(blankId)["nodes"].toArray().size() == 1,
          "choosing a palette item inserts and selects a node");
    const auto initialNodes = preview.blueprint(blankId)["nodes"].toArray();
    QString firstNodeId;
    if (canvas->isVisible() && !initialNodes.isEmpty()) {
        const auto node = initialNodes.first().toObject();
        firstNodeId = node["id"].toString();
        const auto port = node["out"].toArray().first().toObject();
        auto *tree = nodeTree(panel);
        check(tree && tree->topLevelItemCount() == 1 && tree->currentItem() == tree->topLevelItem(0),
              "the new node appears selected in the workflow tree");
        check(node["name"].toString() == "Input" && port["name"].toString() == "Payload"
                  && port["type"].toString() == "object",
              "a generic Input accepts a named and typed output without requiring a node name");
        auto *surface = canvas->viewport();
        const QPoint center = surface->rect().center();
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
        QContextMenuEvent context(QContextMenuEvent::Mouse, center, surface->mapToGlobal(center));
        QCoreApplication::sendEvent(surface, &context);
        check(nodeMenu, "right-clicking a node offers configuration, rename, reuse, and deletion");
        drag(surface, center, QPointF(-120, -120), 80);
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
        check(preview.nodeLibrary().size() == 1, "saving a configured node adds a reusable copy to the library");
        if (auto *remove = visibleNamed<QPushButton>(panel, "Delete node")) {
            confirmDelete(&panel);
            remove->click();
            pump();
        }
        check(preview.blueprint(blankId)["nodes"].toArray().isEmpty(), "deleting the final node returns to the empty canvas");
    }

    if (auto *library = visibleNamed<QPushButton>(panel, "Back to library")) library->click();
    pump();
    if (auto *newGraph = visibleButton(panel, "Create new")) newGraph->click();
    pump();
    if (auto *name = visibleNamed<QLineEdit>(panel, "Name")) name->setText("Reuse test");
    if (auto *submit = visibleButton(panel, "Create blueprint")) submit->click();
    pump();
    for (int i = 0; i < 9; ++i)
        preview.saveNodeTemplate(QString("Saved input %1").arg(i + 1), preview.makeNode("input", "input", 0, 0));
    if (auto *add = visibleButton(panel, "Add node")) add->click();
    pump();
    if (auto *savedPicker = visibleNamed<QDialog>(panel, "Add node picker")) {
        if (auto *tabs = savedPicker->findChild<QTabWidget *>()) tabs->setCurrentIndex(1);
        pump();
        auto *next = visibleButton(*savedPicker, "Next");
        check(next && next->isEnabled() && visibleText(*savedPicker, "Page 1 of 2") &&
              !visibleNamed<QPushButton>(*savedPicker, "Add saved node Object input") &&
              !savedPicker->findChild<QScrollArea *>(), "the saved-node grid pages through larger libraries");
        if (next) next->click();
        pump();
        check(visibleText(*savedPicker, "Page 2 of 2") && next && !next->isEnabled(),
              "the next page shows the remaining cards and stops at the end");
    }
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
    preview.saveBlueprint(preview.example());
    if (auto *library = visibleNamed<QPushButton>(panel, "Back to library")) library->click();
    for (auto *button : panel.findChildren<QPushButton *>()) {
        if (button->isVisible() && button->text() == "Open" && visibleText(*button->parentWidget(), "Order intake")) {
            button->click();
            break;
        }
    }
    if (auto *tree = nodeTree(panel))
        tree->setCurrentItem(tree->topLevelItem(1));
    pump();
    auto *adapter = visibleNamed<QComboBox>(panel, "Node adapter");
    check(canvas->isVisible() && visibleText(panel, "Node inspector") && adapter &&
              adapter->currentText() == "order-selector-v2" && adapter->findData("extractor") < 0,
          "a saved legacy graph opens and shows its model inspector");
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
        auto *surface = canvas->viewport();
        const QPointF point = surface->rect().center();
        int percent = 0;
        const auto observation = QObject::connect(canvas, &BlueprintView::zoomChanged, &panel, [&](int value) { percent = value; });
        canvas->fit();
        const int fitted = percent;
        QWheelEvent wheel(point, surface->mapToGlobal(point.toPoint()), {}, QPoint(0, 120),
            Qt::NoButton, Qt::NoModifier, Qt::NoScrollPhase, false);
        QCoreApplication::sendEvent(surface, &wheel);
        check(wheel.isAccepted() && percent > fitted, "wheel zooms the fullscreen canvas");
        const auto before = canvas->centerInGraph();
        const auto savedBeforePan = preview.saved();
        const auto nodes = preview.example()["nodes"].toArray();
        std::optional<QPointF> background;
        for (int y = 4; y < surface->height() && !background; y += 12) {
            for (int x = 4; x < surface->width(); x += 12) {
                const QPointF candidate(x, y);
                const auto position = canvas->mapToScene(candidate.toPoint());
                if (std::none_of(nodes.cbegin(), nodes.cend(), [&](const auto &node) {
                    return BlueprintScene::nodeRect(node.toObject()).adjusted(-16, -8, 16, 8).contains(position);
                })) {
                    background = candidate;
                    break;
                }
            }
        }
        if (background) drag(surface, *background, *background + QPointF(63, 29));
        check(background && before != canvas->centerInGraph() && savedBeforePan == preview.saved(),
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
    if (auto *runButton = visibleButton(panel, "Run")) runButton->click();
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

    ConversationRunPanel chat(&preview, ConversationRunPanel::Mode::Tool);
    chat.resize(900, 650);
    chat.show();
    chat.activate();
    auto *message = visibleNamed<QPlainTextEdit>(chat, "Task message");
    if (message) message->setPlainText("Show the Python version.");
    if (auto *send = visibleButton(chat, "Send ↑")) send->click();
    const auto chats = preview.chatRuns();
    const auto toolId = chats.isEmpty() ? QString{} : chats.last().toObject()["id"].toString();
    check(!toolId.isEmpty() && waitForState(preview, toolId, "waiting") && visibleText(chat, "--version"),
          "tool chat pauses at an exact command fixture");
    check(chat.findChild<QFrame *>("runToolCall"), "Tool mode shows an inline tool activity block");
    capture(chat, "tool-task-approval");
    panel.openRun(toolId);
    pump();
    check(visibleText(panel, "Working directory") && moreActions(panel).contains("Back to Conversation"),
          "the exact chat run shares the approval view in Blueprints");
    if (auto *deny = visibleButton(panel, "Deny")) deny->click();
    pump();
    check(preview.run(toolId)["status"].toString() == "denied" && visibleText(chat, "The action was denied"),
          "denial updates the shared run and the chat result");
    {   // Both conversation modes keep sessions: New starts a branded blank screen, history reopens a saved run.
        const auto logoShown = [](QWidget &host) {
            auto *logo = host.findChild<QWidget *>("conversationLogo");
            return logo && logo->isVisible();
        };
        if (auto *fresh = visibleButton(chat, "New task")) fresh->click();
        pump();
        check(logoShown(chat) && visibleText(chat, "What would you like to do?"), "a new task shows the Magnite logo");
        ConversationRunPanel flows(&preview, ConversationRunPanel::Mode::Blueprint);
        flows.resize(900, 650);
        flows.show();
        flows.activate();
        pump();
        check(logoShown(flows) && visibleText(flows, "Run a saved workflow") && !flows.findChild<QWidget *>("conversationWorkflowRun"),
              "Blueprint mode opens on a new session with the Magnite logo");
        auto *picker = visibleNamed<QComboBox>(flows, "Saved blueprint");
        if (picker && picker->count() > 1) picker->setCurrentIndex(1);
        pump();
        if (auto *inputs = visibleNamed<QPlainTextEdit>(flows, "Workflow inputs")) inputs->setPlainText("Session check");
        if (auto *run = visibleButton(flows, "Run workflow"); run && run->isEnabled()) run->click();
        pump(120);
        const auto workflowCards = [&flows] {
            int count = 0;
            for (auto *card : flows.findChildren<QWidget *>("conversationWorkflowRun")) count += card->isVisible();
            return count;
        };
        check(workflowCards() == 1 && !logoShown(flows), "running a workflow adds it to the session");
        if (auto *fresh = visibleButton(flows, "New run")) fresh->click();
        pump(400);   // the earlier run keeps reporting progress meanwhile
        check(workflowCards() == 0 && logoShown(flows), "New run starts an empty session that stays empty");
        QTimer::singleShot(150, qApp, [] {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->accept();
        });
        if (auto *history = visibleButton(flows, "Run history")) history->click();
        pump(120);
        check(workflowCards() == 1, "Run history reopens a saved workflow run");
        capture(flows, "conversation-blueprint-session");
        if (auto *fresh = visibleButton(flows, "New run")) fresh->click();
        pump(300);
        capture(flows, "conversation-blueprint-new");
    }
    if (auto *library = visibleNamed<QPushButton>(panel, "Back to library")) library->click();
    pump();
    for (auto *remove : panel.findChildren<QPushButton *>()) {
        if (!remove->isVisible() || remove->accessibleName() != "Delete blueprint" ||
            !visibleText(*remove->parentWidget(), "Blank workflow")) continue;
        confirmDelete(&panel);
        remove->click();
        pump();
        break;
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
    ChatPanel chat;
    auto *tasks = new ConversationRunPanel(&session, ConversationRunPanel::Mode::Tool);
    auto *workflows = new ConversationRunPanel(&session, ConversationRunPanel::Mode::Blueprint);
    chat.setRunPanels(tasks, workflows);
    QObject::connect(workflows, &ConversationRunPanel::runRequested, &panel, &BlueprintPanel::openRun);
    QObject::connect(&panel, &BlueprintPanel::conversationRequested, &chat, &ChatPanel::showBlueprint);
    Theme::setDark(true);
    panel.resize(1000, 680);
    panel.show();
    manager.start();
    if (!check(until([&] { return client.isReady(); }, 35000),
               "native session is connected to the authenticated backend")) {
        fprintf(stderr, "%s\n", qUtf8Printable(manager.recentLogs()));
        return failures;
    }
    chat.setBackendClient(&client);
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
    chat.resize(740, 660);
    chat.show();
    check(visibleNamed<QComboBox>(chat, "Model") && visibleNamed<QPlainTextEdit>(chat, "Chat message"),
          "Chat opens with the local model controls and model test composer");
    capture(chat, "live-conversation-chat-dark");
    if (auto *mode = visibleButton(chat, "Blueprint")) mode->click();
    check(visibleText(chat, "Open your draft in Blueprints") &&
          visibleButton(chat, "Open Blueprints") && !visibleNamed<QComboBox>(chat, "Model"),
          "the Workflow view explains unpublished drafts and links to the library");

    pump();
    if (auto *open = visibleButton(panel, "Open")) open->click();
    pump();
    check(panel.findChild<BlueprintView *>()->isVisible() && visibleText(panel, "Draft"),
          "canvas distinguishes an unpublished draft");
    {
        // New transform nodes: built by the editor's factory, mapped by apiNode, run by the backend.
        const auto runNode = [&](QJsonObject node, const QJsonObject &inputs, const QString &port = "value") -> QJsonValue {
            auto response = std::make_shared<QJsonObject>();
            const QJsonObject graph{{"id", BlueprintPreview::id()}, {"name", "Check new nodes"},
                                    {"nodes", QJsonArray{node}}};
            session.runLive(graph, inputs, node["id"].toString(), false,
                [response](const auto &run, const auto &error) { *response = {{"run", run}, {"error", error}}; });
            waitFor([&] { return !response->isEmpty(); }, 10000);
            const auto id = (*response)["run"].toObject()["id"].toString();
            if (id.isEmpty() || !waitForState(session, id, "passed", 10000)) return QJsonValue(QJsonValue::Undefined);
            return QJsonValue(session.run(id)["result"].toObject()[node["id"].toString()].toObject()[port]);
        };
        auto templated = session.makeNode("template", "note", 0, 0);
        templated["setting"] = "Hello {name}, {{ok}}";
        auto compare = session.makeNode("compare", "large", 0, 0);
        compare["setting"] = "greater";
        auto choose = session.makeNode("choose", "pick", 0, 0);
        auto validate = session.makeNode("validate", "check", 0, 0);
        check(session.settingError(templated).isEmpty() && runNode(templated, {{"name", "Ada"}}).toString() == "Hello Ada, {ok}",
              "Text template fills named inputs and keeps escaped braces");
        templated["setting"] = "Hello {city}";
        check(session.settingError(templated).contains("city"), "Text template names a missing input port");
        check(runNode(compare, {{"left", 5}, {"right", 3}}) == QJsonValue(true)
              && runNode(compare, {{"left", 1}, {"right", 3}}) == QJsonValue(false),
              "Compare evaluates the saved operator");
        check(runNode(choose, {{"condition", false}, {"if_true", "Review"}, {"if_false", "Approved"}}).toString() == "Approved",
              "Choose a value returns the selected input");
        const QJsonArray rows{QJsonObject{{"id", "a"}, {"amount", 150}}, QJsonObject{{"id", "b"}, {"amount", 20}},
                              QJsonObject{{"id", "c"}, {"amount", 900}}, QJsonObject{{"id", "d"}, {"tag", "x"}}};
        auto filter = session.makeNode("filter", "large", 0, 0);
        auto sort = session.makeNode("sort", "ranked", 0, 0);
        const auto ids = [](const QJsonValue &value) {
            QStringList names;
            for (const auto &row : value.toArray()) names << row.toObject()["id"].toString();
            return names.join(",");
        };
        check(BlueprintPreview::parseRule("status = \"open\"")["value"] == "open"
              && BlueprintPreview::parseRule("count != 3")["operator"] == "not-equals"
              && BlueprintPreview::parseRule("tags contains urgent")["value"] == "urgent"
              && BlueprintPreview::ruleText(BlueprintPreview::parseRule("note = \"say \\\"hi\\\"\"")) == "note = \"say \\\"hi\\\"\""
              && session.settingError(filter).isEmpty()
              && !session.settingError([&] { auto bad = filter; bad["setting"] = "amount about 5"; return bad; }()).isEmpty(),
              "Filter rules parse comparisons, text, numbers, and quoted text and reject unknown comparisons");
        check(ids(runNode(filter, {{"rows", rows}}, "rows")) == "a,c", "Filter rows keeps rows that pass the default rule");
        check(ids(runNode(sort, {{"rows", rows}}, "rows")) == "c,a,b,d", "Sort rows orders by the field, largest first, missing last");
        auto knowledge = session.makeNode("search", "docs", 0, 0);
        knowledge["setting"] = "2";
        check(runNode(knowledge, {{"query", "refund"}}, "context").isUndefined(),
              "Knowledge search fails clearly when no documents are indexed");
        // A clean profile picks its embedder by memory; use the small one the selftests cache.
        auto configured = std::make_shared<int>(0);
        client.post("/rag/config", {{"model", "all-MiniLM-L6-v2"}},
            [configured](int status, const QJsonDocument &) { *configured = status ? status : -1; });
        waitFor([&] { return *configured != 0; }, 10000);
        auto ingested = std::make_shared<int>(0);
        client.post("/rag/documents", {{"name", "policy.md"}, {"kind", "md"},
            {"content", "# Refund policy\n\nRefunds are issued within five business days after the item arrives."
                        "\n\n# Shipping\n\nStandard shipping takes three to seven days."}},
            [ingested](int status, const QJsonDocument &) { *ingested = status ? status : -1; });
        if (check(waitFor([&] { return *ingested != 0; }, 90000) && *ingested == 201,
                  "the clean profile indexes a Knowledge document for search")) {
            const auto context = runNode(knowledge, {{"query", "How long do refunds take?"}}, "context").toString();
            check(context.startsWith("[1] policy.md: ") && context.contains("five business days"),
                  "Knowledge search returns cited passages for a Model prompt");
        }
        check(session.settingError(validate).isEmpty(), "Validate data accepts its default rules");
        const auto valid = runNode(validate, {{"value", QJsonObject{{"id", "INV-1"}}}});
        check(valid == QJsonValue(QJsonObject{{"id", "INV-1"}}), "Validate data passes a matching value");
        check(runNode(validate, {{"value", QJsonObject{{"id", ""}}}}).isUndefined(),
              "Validate data fails a value that breaks its rules");
    }

    if (auto *add = visibleButton(panel, "Add node")) add->click();
    pump();
    if (auto *picker = visibleNamed<QDialog>(panel, "Add node picker")) {
        const auto cards = picker->findChildren<QPushButton *>("blueprintNodeCard");
        check(cards.size() == 9 && !picker->findChild<QScrollArea *>() &&
              std::none_of(cards.begin(), cards.end(), [](const auto *card) {
                  return card->accessibleName() == "Add Pi Agent";
              }) &&
              std::any_of(cards.begin(), cards.end(), [](const auto *card) {
                  return card->accessibleName() == "Add Knowledge";
              }) &&
              picker->height() <= picker->screen()->availableGeometry().height() &&
              std::all_of(cards.begin(), cards.end(), [picker](const auto *card) {
                  const auto *title = card->template findChild<QLabel *>("headingLabel");
                  const auto *caption = card->template findChild<QLabel *>("cardMeta");
                  return card->isVisible() && picker->rect().contains(card->mapTo(picker, QPoint())) &&
                         picker->rect().contains(card->mapTo(picker, card->rect().bottomRight())) &&
                         title && card->accessibleName() == "Add " + title->text() &&
                         caption && caption->height() >= caption->heightForWidth(caption->width());
              }), "all nine live cards fit without scrolling, with Knowledge and without Pi Agent");
        capture(*picker, "live-add-node-cards");
        picker->reject();
        pump();
    } else check(false, "the live node picker opens");
    capture(panel, "live-canvas-dark");
    QSettings().setValue("appearance/studio3ReducedMotion", true);
    panel.resize(740, 620);
    Theme::setDark(false);
    pump();
    check(panel.width() == 740 && panel.findChild<BlueprintView *>()->isVisible(),
          "compact light canvas fits with reduced motion enabled");
    capture(panel, "live-canvas-compact-light");

    if (auto *tree = nodeTree(panel); tree && tree->topLevelItemCount()) tree->setCurrentItem(tree->topLevelItem(0));
    pump();
    if (auto *test = visibleButton(panel, "Test this node")) test->click();
    if (check(until([&] {
        const auto *start = visibleButton(panel, "Test node");
        return start && start->isEnabled() && visibleNamed<QPlainTextEdit>(panel, "Run input");
    }), "the isolated draft test exposes a typed input editor")) {
        auto *message = visibleNamed<QPlainTextEdit>(panel, "Run input");
        message->setPlainText(R"({"value":{"message":"isolated test"}})");
        visibleButton(panel, "Test node")->click();
        check(until([&] { return visibleText(panel, "isolated test"); }, 15000),
              "isolated draft node returns the supplied typed input");
    }

    if (auto *save = visibleButton(panel, "Publish")) save->click();
    if (!check(until([&] { return !session.published().isEmpty() && visibleText(panel, "Revision 1"); }),
               "saving publishes a backend revision from the native editor")) return failures;
    check(until([&] {
        const auto *picker = visibleNamed<QComboBox>(chat, "Saved blueprint");
        return picker && picker->currentData().toString() == graphId && visibleNamed<QPlainTextEdit>(chat, "Workflow inputs");
    }), "the open Conversation picker updates when the draft is published");
    if (auto *mode = visibleButton(chat, "Tool mode")) mode->click();
    auto *taskSend = visibleButton(chat, "Send ↑");
    if (auto *message = visibleNamed<QPlainTextEdit>(chat, "Task message")) message->setPlainText("Help with a task.");
    check(until([&] { return visibleText(chat, "Load a model first"); }) &&
          visibleNamed<QComboBox>(chat, "Model") && taskSend && !taskSend->isEnabled(),
          "Tool mode offers a model picker and disables sending without a loaded model");
    capture(chat, "live-conversation-tool-light");
    if (auto *open = visibleButton(panel, "Open in Conversation")) open->click();
    auto *picker = visibleNamed<QComboBox>(chat, "Saved blueprint");
    auto *send = visibleButton(chat, "Run workflow");
    check(picker && picker->currentData().toString() == graphId && picker->currentText().contains("Rev 1") &&
          until([&] { return visibleNamed<QPlainTextEdit>(chat, "Workflow inputs"); }) &&
          !visibleNamed<QPlainTextEdit>(chat, "Task message"),
          "opening in Conversation selects the saved revision and its Workflow inputs");
    // Dirty edits to the editor must not change what runs from chat.
    auto changed = draft;
    changed["revision"] = 1;
    changed["name"] = "Unpublished edits";
    session.saveBlueprint(changed);
    const auto openRequest = QString::fromUtf8(R"({"request":{"action":"open","target":"https://example.com/blueprint-check"}})");
    if (auto *message = visibleNamed<QPlainTextEdit>(chat, "Workflow inputs")) message->setPlainText(openRequest);
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
    check(until([&] {
        const auto *deny = visibleButton(panel, "Deny");
        return deny && !deny->isEnabled();
    }), "pending decisions are unavailable while disconnected");
    client.setBaseUrl(manager.baseUrl());
    check(until([&] { return visibleButton(panel, "Deny") && visibleButton(panel, "Deny")->isEnabled(); }),
          "the pending decision is restored on reconnect");
    if (auto *deny = visibleButton(panel, "Deny")) deny->click();
    check(waitForState(session, runId, "failed") && visibleText(chat, "Denied") && visibleText(panel, "Tool action ended"),
          "denial updates the same durable run on both surfaces");
    if (auto *details = visibleButton(chat, "Show action details")) details->click();
    check(visibleText(chat, "https://example.com/blueprint-check"),
          "a settled activity block can reopen the action details reviewed in this session");
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
    const auto adapterModel = qEnvironmentVariable("MAGNITE_BLUEPRINTS_ACCEPTANCE_MODEL");
    if (!adapterModel.isEmpty())
        check(runModelBlueprintAcceptance(session, panel, chat, adapterModel, testModel, captures),
              "clean-profile model and adapter acceptance");
    if (!testModel.isEmpty()) {
        struct CheckpointCheck { QJsonObject info; QString error; bool ready = false; };
        auto checkpoint = std::make_shared<CheckpointCheck>();
        session.checkpoint(modelId(testModel), [checkpoint](const auto &info, const auto &error) {
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
            chat.refreshEngineStatus();
            if (auto *mode = visibleButton(chat, "Tool mode")) mode->click();
            if (auto *message = visibleNamed<QPlainTextEdit>(chat, "Task message"))
                message->setPlainText("Reply with the word ready. No external actions are needed.");
            if (until([&] { return taskSend && taskSend->isEnabled(); })) taskSend->click();
            check(until([&] {
                const auto runs = session.chatRuns();
                const auto last = runs.last().toObject();
                return last["toolChat"].toBool() && last["status"] == "passed" && last["result"].isString();
            }, 30000), "native Tool mode sends an ad hoc task through Pi and displays its answer");
            capture(chat, "live-conversation-tool-reply");
            if (auto *message = visibleNamed<QPlainTextEdit>(chat, "Task message"))
                message->setPlainText("Say ready again.");
            if (until([&] { return taskSend && taskSend->isEnabled(); })) taskSend->click();
            check(until([&] {
                const auto runs = session.chatRuns();
                if (runs.isEmpty()) return false;
                const auto last = runs.last().toObject();
                const auto messages = last["messages"].toArray();
                return last["toolChat"].toBool() && last["status"] == "passed" && messages.size() == 3 &&
                    messages.at(1).toObject()["role"] == "assistant" &&
                    messages.at(2).toObject()["content"] == "Say ready again.";
            }, 30000), "a Pi follow-up sends the previous prompt and answer as context");


            if (auto *back = visibleNamed<QPushButton>(panel, "Back to library")) back->click();
            if (auto *triage = visibleButton(panel, "Request triage example")) triage->click();
            const auto triageDraft = [&] {
                for (const auto &value : session.saved())
                    if (value.toObject()["name"] == "Request triage") return value.toObject()["id"].toString();
                return QString{};
            }();
            if (check(!triageDraft.isEmpty() && until([&] { return visibleButton(panel, "Publish"); }),
                      "the desktop opens the editable request triage example")) {
                // The example's Model node starts unbound; bind the cached checkpoint and reopen it.
                QJsonObject draft;
                for (const auto &value : session.saved())
                    if (value.toObject()["id"] == triageDraft) draft = value.toObject();
                auto nodes = draft["nodes"].toArray();
                for (int i = 0; i < nodes.size(); ++i) {
                    auto entry = nodes.at(i).toObject();
                    if (entry["id"] == "triage") entry["model"] = modelId(testModel);
                    nodes.replace(i, entry);
                }
                draft["nodes"] = nodes;
                session.saveBlueprint(draft);
                if (auto *back = visibleNamed<QPushButton>(panel, "Back to library")) back->click();
                pump();
                for (auto *button : panel.findChildren<QPushButton *>()) {
                    if (button->isVisible() && button->text() == "Open"
                        && visibleText(*button->parentWidget(), "Request triage")) {
                        button->click();
                        break;
                    }
                }
                until([&] { return visibleButton(panel, "Publish"); });
                pump();
                capture(panel, "live-triage-graph");
                visibleButton(panel, "Publish")->click();
                if (check(until([&] { return visibleText(panel, "Revision 1"); }),
                          "the Model node, constant, and object builder publish together")) {
                    if (auto *tests = visibleButton(panel, "Test cases")) tests->click();
                    if (auto *message = visibleNamed<QPlainTextEdit>(panel, "Run input"))
                        message->setPlainText("Please send replacement labels for order 245.");
                    if (auto *start = visibleButton(panel, "Start run")) start->click();
                    check(until([&] {
                        const auto runs = session.runs(triageDraft);
                        const auto run = runs.isEmpty() ? QJsonObject{} : runs.first().toObject();
                        const auto record = run["result"].toObject()["result"].toObject();
                        return run["status"] == "passed" && record["status"] == "Needs review" &&
                               !record["summary"].toString().trimmed().isEmpty() && run["decisions"].toArray().isEmpty();
                    }, 60000), "the triage Model node returns text and builds the review record");
                    capture(panel, "live-triage-result");
                    if (auto *history = visibleButton(panel, "Run history")) history->click();
                    pump(300);
                    capture(panel, "live-run-history");
                    if (auto *mode = visibleButton(chat, "Blueprint")) mode->click();
                    pump(500);
                    capture(chat, "live-conversation-blueprint");
                }
            }
        }
    }

    {
        // Dialog tour: open each editor dialog on a representative draft and capture it for review.
        QJsonObject tour{{"id", BlueprintPreview::id()}, {"revision", 0}, {"dirty", true}, {"name", "Dialog tour"},
                         {"description", ""}, {"edges", QJsonArray{}}};
        const QStringList operations{"generate", "reason", "dataset-rows", "search", "filter", "compare", "template"};
        QJsonArray nodes;
        for (int i = 0; i < operations.size(); ++i)
            nodes.append(session.makeNode(operations[i], QString("n%1").arg(i), (i % 3) * 230, (i / 3) * 170));
        tour["nodes"] = nodes;
        session.saveBlueprint(tour);
        if (auto *back = visibleNamed<QPushButton>(panel, "Back to library")) back->click();
        pump();
        for (auto *button : panel.findChildren<QPushButton *>()) {
            if (button->isVisible() && button->text() == "Open" && visibleText(*button->parentWidget(), "Dialog tour")) {
                button->click();
                break;
            }
        }
        pump();
        const auto captureDialog = [&](const QString &name, const std::function<void()> &trigger) {
            bool seen = false;
            QTimer poll;
            poll.setInterval(150);
            QElapsedTimer opened;
            const auto connection = QObject::connect(&poll, &QTimer::timeout, &panel, [&] {
                auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                if (!dialog) return;
                if (!opened.isValid()) opened.start();
                if (opened.elapsed() < 1200) return;  // Let live checkpoint and dataset options arrive.
                capture(*dialog, name);
                seen = true;
                dialog->reject();
            });
            poll.start();
            trigger();
            poll.stop();
            QObject::disconnect(connection);
            return seen;
        };
        auto *tree = nodeTree(panel);
        int captured = 0;
        for (int i = 0; tree && i < operations.size(); ++i) {
            tree->setCurrentItem(tree->topLevelItem(i));
            pump();
            captured += captureDialog("dialog-configure-" + operations[i], [&] {
                if (auto *configure = visibleButton(panel, "Configure…")) configure->click();
            });
            if (operations[i] == "generate" || operations[i] == "reason")
                captured += captureDialog("dialog-binding-" + operations[i], [&] {
                    if (auto *binding = visibleButton(panel, "Change binding")) binding->click();
                });
            if (operations[i] == "dataset-rows") {
                captured += captureDialog("dialog-dataset", [&] {
                    if (auto *source = visibleButton(panel, "Choose dataset")) source->click();
                });
                captured += captureDialog("dialog-schema", [&] {
                    for (auto *button : panel.findChildren<QPushButton *>("blueprintPortContract"))
                        if (button->isVisible()) { button->click(); break; }
                });
            }
        }
        capture(panel, "dialog-tour-canvas");
        check(captured == int(operations.size()) + 4, "every editor dialog opens and closes in the dialog tour");
        session.deleteBlueprint(tour["id"].toString());
        if (auto *back = visibleNamed<QPushButton>(panel, "Back to library")) back->click();
        pump();
    }

    client.abortAll();
    client.setBaseUrl({});
    manager.stop();
    fprintf(stderr, "[blueprints-live] %s (%d failures)\n", failures ? "FAIL" : "PASS", failures);
    return failures;
}
