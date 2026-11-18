#include "panels/blueprintpanel.h"

#include "panels/blueprintpreview.h"
#include "panels/blueprintnodedialog.h"
#include "theme.h"
#include "widgets/blueprintcanvas.h"
#include "widgets/button.h"
#include "widgets/combobox.h"
#include "widgets/confirmation.h"
#include "widgets/edgescrollbar.h"
#include "widgets/settingswindow.h"
#include "widgets/surface.h"
#include "widgets/tabwidget.h"

#include <QApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QScrollArea>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QShortcut>
#include <QStackedWidget>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <utility>

namespace {
QString literalButtonText(QString text) { return text.replace('&', "&&"); }

QLabel *label(const QString &text, const char *style = nullptr) {
    auto *result = new QLabel(text);
    result->setTextFormat(Qt::PlainText);
    if (style) result->setObjectName(style);
    return result;
}

QLabel *wrapped(const QString &text, const char *style = "cardMeta") {
    auto *result = label(text, style);
    result->setWordWrap(true);
    result->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return result;
}

Button *action(const QString &text, bool primary = false, bool flat = false) {
    auto *result = new Button(literalButtonText(text));
    result->setProperty("primary", primary);
    result->setFlat(flat);
    return result;
}

Button *iconAction(const QString &icon, const QString &name) {
    auto *button = action({}, false, true);
    button->setObjectName("blueprintIconButton");
    button->setAccessibleName(name);
    button->setToolTip(name);
    button->setFixedSize(32, 32);
    button->setProperty("themeIcon", icon);
    button->setIcon(Theme::coloredIcon(icon, QColor(Theme::textSecondary()), 16));
    return button;
}

void clear(QLayout *layout) {
    while (auto *item = layout->takeAt(0)) {
        if (auto *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        } else if (auto *child = item->layout()) clear(child);
        delete item;
    }
}

QFrame *rule() {
    auto *line = new QFrame;
    line->setObjectName("blueprintRule");
    line->setFixedHeight(1);
    return line;
}

QScrollArea *scroller(QVBoxLayout **body) {
    auto *scroll = new QScrollArea;
    auto *content = new QWidget;
    *body = new QVBoxLayout(content);
    (*body)->setContentsMargins(0, 0, 6, 0);
    (*body)->setSpacing(0);
    scroll->setWidget(content);
    scroll->setWidgetResizable(true);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    EdgeScrollBar::attach(scroll);
    return scroll;
}

QJsonObject findById(const QJsonArray &array, const QString &id) {
    for (const auto &v : array) {
        const auto item = v.toObject();
        if (item["id"].toString() == id) return item;
    }
    return {};
}

QString sampleText(const QJsonObject &sample) {
    QStringList lines;
    for (const auto &line : sample["lines"].toArray()) lines.append(line.toString());
    return lines.join(' ');
}

QString named(const QJsonObject &fixtures, const QString &collection, const QJsonValue &id) {
    const auto object = findById(fixtures[collection].toArray(), id.toString());
    return object["name"].toString("Unbound");
}

void titleBar(QVBoxLayout *body, const QString &title) {
    body->addWidget(label(title, "headingLabel"));
    body->addWidget(rule());
}

QDialogButtonBox *dialogButtons(QDialog *dialog, QVBoxLayout *body, const QString &submit) {
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel, dialog);
    buttons->addButton(submit, QDialogButtonBox::AcceptRole);
    QObject::connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    body->addWidget(buttons);
    return buttons;
}

QColor statusTone(const QString &status) {
    if (status == "passed" || status == "approved") return QColor(Theme::statusSuccess());
    if (status == "failed" || status == "denied") return QColor(Theme::statusError());
    if (status == "waiting" || status == "expired") return QColor(Theme::statusWarning());
    if (status == "running") return QColor(Theme::comparisonSeries());
    return QColor(Theme::textMuted());
}

QString statusIcon(const QString &status) {
    if (status == "passed" || status == "approved") return "check";
    if (status == "failed" || status == "denied") return "window-close";
    if (status == "running") return "play";
    if (status == "queued" || status == "waiting") return "square";
    return "window-minimize";
}

QLabel *statusLabel(const QString &status) {
    const char *style = "cardMeta";
    if (status == "passed" || status == "approved") style = "blueprintSuccess";
    else if (status == "failed" || status == "denied") style = "blueprintError";
    else if (status == "waiting" || status == "expired") style = "blueprintWarning";
    else if (status == "running") style = "blueprintRunning";
    return label(BlueprintPreview::statusLabel(status), style);
}

bool running(const QJsonObject &run) {
    const auto status = run["status"].toString();
    return status == "running" || status == "waiting" || status == "queued";
}

QJsonArray decisionsFor(const QJsonObject &run) {
    if (run["decisions"].isArray()) return run["decisions"].toArray();
    return run["decision"].isObject() ? QJsonArray{run["decision"]} : QJsonArray{};
}

QWidget *approvalView(BlueprintPreview *preview, const QJsonObject &run, const QJsonObject &decision) {
    auto *pane = new QWidget;
    auto *body = new QVBoxLayout(pane);
    body->setContentsMargins(0, 0, 0, 4);
    const auto status = decision["status"].toString();
    auto *header = new QHBoxLayout;
    header->addWidget(label(decision["type"].toString(), "headingLabel"));
    header->addStretch();
    header->addWidget(statusLabel(status));
    body->addLayout(header);
    if (decision.contains("executable")) {
        body->addWidget(wrapped("Executable  " + BlueprintPreview::jsonText(decision["executable"]), "blueprintCode"));
        body->addWidget(wrapped("Arguments  " + BlueprintPreview::jsonText(decision["args"]), "blueprintCode"));
        body->addWidget(wrapped("Working directory  " + BlueprintPreview::jsonText(decision["cwd"]), "blueprintCode"));
    } else if (decision.contains("target"))
        body->addWidget(wrapped("Target  " + BlueprintPreview::jsonText(decision["target"]), "blueprintCode"));
    if (status == "waiting") {
        const auto expiry = decision["expires"].toInteger();
        auto *actions = new QHBoxLayout;
        auto *expires = label({}, "cardMeta");
        auto update = [expiry, expires] {
            expires->setText(QString("Expires in %1s")
                .arg(qMax(qint64(0), (expiry - QDateTime::currentMSecsSinceEpoch() + 999) / 1000)));
        };
        auto *timer = new QTimer(expires);
        QObject::connect(timer, &QTimer::timeout, expires, update);
        timer->start(250);
        update();
        auto *deny = action("Deny");
        auto *approve = action("Approve once", true);
        const auto id = run["id"].toString();
        for (auto *button : {deny, approve}) {
            button->setProperty("approvalFocus", id + decision["id"].toString() + button->text());
            button->setEnabled(!preview->live() || preview->connected());
            QObject::connect(button, &Button::clicked, preview, [preview, id, decision, button, approve] {
                preview->decide(id, button == approve, decision);
            });
        }
        actions->addWidget(expires);
        actions->addStretch();
        actions->addWidget(deny);
        actions->addWidget(approve);
        body->addLayout(actions);
    } else {
        static const QHash<QString, QString> notes{{"approved", "The preview recorded a simulated approval."},
            {"denied", "The action was denied and the run stopped."}, {"expired", "The approval window expired and the run stopped."},
            {"cancelled", "The approval request was cancelled."}};
        if (!preview->live()) body->addWidget(wrapped(notes.value(status)));
    }
    return pane;
}

QString inputHint(const QJsonObject &contracts, bool nodeOnly) {
    if (contracts.isEmpty()) return "This run needs no inputs.";
    if (contracts.size() == 1 && contracts.begin().value().toObject()["type"] == "string")
        return QString("Enter text for %1.").arg(contracts.begin().key());
    QStringList fields;
    for (auto it = contracts.begin(); it != contracts.end(); ++it)
        fields << it.key() + " (" + it.value().toObject()["type"].toString() + ")";
    return QString("Enter a JSON object keyed by %1: %2.")
        .arg(nodeOnly ? "input port" : "Input node", fields.join(", "));
}

QJsonObject collectInput(const QJsonObject &contracts, const QString &text, QString *error) {
    if (contracts.isEmpty()) return {};
    if (contracts.size() == 1 && contracts.begin().value().toObject()["type"] == "string")
        return {{contracts.begin().key(), text}};
    const auto doc = QJsonDocument::fromJson(text.toUtf8());
    if (!doc.isObject()) { *error = "Enter a JSON object for the shown inputs."; return {}; }
    const auto input = doc.object();
    if (input.keys() != contracts.keys()) {
        *error = "Provide exactly these keys: " + contracts.keys().join(", ") + ".";
        return {};
    }
    return input;
}

void fillCode(QVBoxLayout *layout, const QString &title, const QJsonValue &value) {
    layout->addWidget(label(title, "headingLabel"));
    auto *code = wrapped(BlueprintPreview::jsonText(value), "blueprintCode");
    code->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    layout->addWidget(code);
    layout->addStretch();
}
}

BlueprintPanel::BlueprintPanel(BlueprintPreview *preview, QWidget *parent)
    : QWidget(parent), m_preview(preview), m_dark(Theme::isDark()), m_pages(new QStackedWidget(this)),
      m_library(new QWidget), m_workspace(new QWidget) {
    setObjectName("blueprintsWorkspace");
    setAttribute(Qt::WA_StyledBackground, true);
    auto *root = new QVBoxLayout(this);
    root->setContentsMargins(20, 17, 20, 14);
    root->addWidget(m_pages);
    m_pages->addWidget(m_library);
    m_pages->addWidget(m_workspace);
    setupCreatePage();
    auto *library = new QVBoxLayout(m_library);
    library->setContentsMargins(0, 0, 0, 0);
    auto *exampleRow = new QHBoxLayout;
    exampleRow->addStretch();
    auto *example = action("Order intake example", false, true);
    example->setObjectName("blueprintExample");
    exampleRow->addWidget(example);
    connect(example, &Button::clicked, this, [this] { openBlueprint("order-intake"); });
    if (m_preview->live()) {
        auto *recent = action("Recent runs", false, true);
        exampleRow->addWidget(recent);
        connect(recent, &Button::clicked, this, [this] {
            SettingsDetailDialog dialog(this);
            dialog.setWindowTitle("Recent runs");
            auto *body = new QVBoxLayout(&dialog);
            titleBar(body, dialog.windowTitle());
            auto *runs = new QListWidget;
            runs->setAccessibleName("Recent Blueprint and chat runs");
            for (const auto &value : m_preview->recentRuns()) {
                const auto run = value.toObject();
                const auto time = QDateTime::fromMSecsSinceEpoch(run["created"].toInteger()).toString("d MMM, h:mm AP");
                auto *item = new QListWidgetItem(run["name"].toString() + " · " +
                    BlueprintPreview::statusLabel(run["status"].toString()) + " · " + time, runs);
                item->setData(Qt::UserRole, run["id"].toString());
            }
            if (!runs->count()) body->addWidget(wrapped("Start a run to see it here."));
            runs->setCurrentRow(0);
            body->addWidget(runs);
            const auto buttons = dialogButtons(&dialog, body, "Inspect run");
            for (auto *button : buttons->buttons())
                if (buttons->buttonRole(button) == QDialogButtonBox::AcceptRole) button->setEnabled(runs->count() > 0);
            connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
            connect(runs, &QListWidget::itemActivated, &dialog, &QDialog::accept);
            dialog.resize(480, 330);
            if (dialog.exec() == QDialog::Accepted && runs->currentItem())
                openRun(runs->currentItem()->data(Qt::UserRole).toString());
        });
    }
    library->addLayout(exampleRow);
    m_service = wrapped({}, "blueprintWarning");
    m_service->hide();
    library->addWidget(m_service);
    library->addWidget(scroller(&m_libraryBody), 1);

    auto *workspace = new QVBoxLayout(m_workspace);
    workspace->setContentsMargins(0, 0, 0, 0);
    auto *crumb = new QHBoxLayout;
    auto *back = action("‹ Blueprints", false, true);
    back->setAccessibleName("Blueprint library");
    crumb->addWidget(back);
    crumb->addStretch();
    m_backToChat = action("Back to chat", false, true);
    m_backToChat->hide();
    crumb->addWidget(m_backToChat);
    connect(m_backToChat, &Button::clicked, this, &BlueprintPanel::chatRequested);
    if (!m_preview->live()) crumb->addWidget(label("Fixture data", "cardMeta"));
    m_publish = action("Save revision", false, true);
    m_reload = action("Reload saved revision", false, true);
    m_publish->hide();
    m_reload->hide();
    crumb->addWidget(m_reload);
    crumb->addWidget(m_publish);
    connect(m_publish, &Button::clicked, this, &BlueprintPanel::publish);
    connect(m_reload, &Button::clicked, this, &BlueprintPanel::reloadRevision);
    workspace->addLayout(crumb);
    connect(back, &Button::clicked, this, &BlueprintPanel::showLibrary);

    auto *head = new QHBoxLayout;
    auto *heading = new QVBoxLayout;
    auto *name = new QHBoxLayout;
    m_title = wrapped({}, "blueprintTitle");
    m_title->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
    m_revision = label({}, "blueprintRevision");
    name->addWidget(m_title);
    name->addWidget(m_revision, 0, Qt::AlignVCenter);
    name->addStretch();
    heading->addLayout(name);
    m_description = wrapped({});
    heading->addWidget(m_description);
    head->addLayout(heading, 1);
    auto *edit = iconAction("pencil", "Edit blueprint details");
    edit->setObjectName("blueprintEdit");
    edit->setAccessibleName("Edit blueprint details");
    auto *copy = iconAction("copy", "Duplicate blueprint");
    copy->setObjectName("blueprintDuplicate");
    copy->setAccessibleName("Duplicate blueprint");
    auto *remove = iconAction("trash", "Delete blueprint");
    remove->setObjectName("blueprintDelete");
    auto *run = action("Run blueprint", true);
    head->addWidget(edit);
    head->addWidget(copy);
    head->addWidget(remove);
    head->addWidget(run);
    workspace->addLayout(head);
    connect(edit, &Button::clicked, this, &BlueprintPanel::editDetails);
    connect(remove, &Button::clicked, this, [this] { deleteBlueprint(m_graph["id"].toString()); });
    connect(copy, &Button::clicked, this, [this] { if (!m_graph.isEmpty()) duplicate(m_graph); });
    connect(run, &Button::clicked, this, [this] { showTest(); });

    m_tabs = new TabWidget;
    m_tabs->tabBar()->setProperty("underline", true);
    workspace->addWidget(m_tabs, 1);
    setupEditor();
    setupRun();
    m_tabs->addTab(scroller(&m_history), "Run history");
    connect(m_tabs, &TabWidget::currentChanged, this, [this, run](int index) {
        run->setVisible(index != 1 && !m_graph.isEmpty());
        if (index == 2) {
            refreshHistory();
            m_preview->refreshHistory(m_graph["id"].toString());
        }
    });
    connect(m_preview, &BlueprintPreview::savedChanged, this, &BlueprintPanel::refreshLibrary);
    connect(m_preview, &BlueprintPreview::nodeLibraryChanged, this, &BlueprintPanel::refreshNodePalette);
    connect(m_preview, &BlueprintPreview::runChanged, this, [this](const QString &id) {
        if (id == m_runId) refreshRun();
        if (m_tabs->currentIndex() == 2) refreshHistory();
    });
    connect(m_preview, &BlueprintPreview::serviceChanged, this, [this] {
        const auto error = m_preview->serviceError();
        m_service->setText(error);
        m_service->setVisible(!error.isEmpty());
        if (m_preview->live()) {
            m_publish->setEnabled(m_preview->connected());
            m_reload->setEnabled(m_preview->connected());
            if (!m_graph.isEmpty()) refreshRunOptions();
            refreshRun();
        }
    });
    connect(m_preview, &BlueprintPreview::optionsChanged, this, [this] {
        if (!m_graph.isEmpty()) refreshRunOptions();
    });
    if (m_preview->live()) {
        m_service->setText(m_preview->serviceError());
        m_service->setVisible(!m_service->text().isEmpty());
    }
    showLibrary();
}

void BlueprintPanel::showLibrary() {
    ++m_navigation;
    refreshLibrary();
    m_pages->setCurrentWidget(m_library);
}

void BlueprintPanel::refreshLibrary() {
    clear(m_libraryBody);
    const auto saved = m_preview->saved();
    bool empty = true;
    for (const auto &value : saved) empty &= value.toObject()["id"].toString() == "order-intake";
    auto *create = action("Create new", true);
    create->setObjectName("blueprintCreate");
    create->setIcon(Theme::coloredIcon("plus", QColor(Theme::accentText()), 16));
    connect(create, &Button::clicked, this, &BlueprintPanel::showCreatePage);
    if (empty) {
        m_libraryBody->addStretch();
        m_libraryBody->addWidget(label("Blueprints", "blueprintTitle"), 0, Qt::AlignHCenter);
        m_libraryBody->addSpacing(14);
        m_libraryBody->addWidget(create, 0, Qt::AlignHCenter);
        m_libraryBody->addStretch();
        return;
    }
    auto *header = new QHBoxLayout;
    header->addWidget(label("Blueprints", "blueprintTitle"), 1);
    header->addWidget(create);
    m_libraryBody->addLayout(header);
    m_libraryBody->addSpacing(16);
    m_libraryBody->addWidget(label(m_preview->live() ? "Drafts and saved blueprints" : "Saved blueprints", "headingLabel"));
    for (const auto &value : saved) {
        const auto graph = value.toObject();
        const auto id = graph["id"].toString();
        if (id == "order-intake") continue;
        auto *card = Surface::card(m_library);
        auto *row = new QHBoxLayout;
        auto *text = new QVBoxLayout;
        text->addWidget(label(graph["name"].toString(), "headingLabel"));
        text->addWidget(wrapped(graph["description"].toString()));
        const auto recent = m_preview->runs(id);
        const auto revision = graph["revision"].toInt();
        auto detail = revision ? QString("Revision %1").arg(revision) : QString("Draft");
        if (revision && graph["dirty"].toBool()) detail += " · Draft edits";
        if (graph["nodes"].isArray()) detail += QString(" · %1 nodes").arg(graph["nodes"].toArray().size());
        if (!recent.isEmpty()) detail += " · " + BlueprintPreview::statusLabel(recent.first().toObject()["status"].toString());
        text->addWidget(wrapped(detail));
        row->addLayout(text, 1);
        auto *duplicateButton = iconAction("copy", "Duplicate blueprint");
        auto *remove = iconAction("trash", "Delete blueprint");
        auto *open = action("Open");
        row->addWidget(duplicateButton);
        row->addWidget(remove);
        row->addWidget(open);
        connect(remove, &Button::clicked, this, [this, id] { deleteBlueprint(id); });
        card->addLayout(row);
        m_libraryBody->addSpacing(7);
        m_libraryBody->addWidget(card->parentWidget());
        connect(open, &Button::clicked, this, [this, id] { openBlueprint(id); });
        connect(duplicateButton, &Button::clicked, this, [this, graph, id] {
            if (!m_preview->live() || graph["nodes"].isArray()) { duplicate(graph); return; }
            m_preview->getBlueprint(id, false, [self = QPointer(this)](const auto &saved, const auto &error) {
                if (!self) return;
                if (!error.isEmpty()) Ui::showNotice(self, "Blueprints", error);
                else self->duplicate(saved);
            });
        });
    }
    m_libraryBody->addStretch();
}

void BlueprintPanel::setupCreatePage() {
    m_createPage = new QWidget;
    m_createPage->setObjectName("blueprintCreatePage");
    m_pages->addWidget(m_createPage);
    auto *page = new QVBoxLayout(m_createPage);
    page->setContentsMargins(0, 0, 0, 0);
    auto *back = action("‹ Blueprints", false, true);
    back->setAccessibleName("Back to blueprint library");
    page->addWidget(back, 0, Qt::AlignLeft);
    connect(back, &Button::clicked, this, &BlueprintPanel::showLibrary);
    page->addStretch(2);
    auto *form = Surface::card(m_createPage);
    form->parentWidget()->setObjectName("blueprintCreateForm");
    form->parentWidget()->setFixedWidth(430);
    form->setContentsMargins(22, 22, 22, 20);
    form->setSpacing(9);
    form->addWidget(label("New blueprint", "blueprintTitle"));
    form->addSpacing(5);
    m_newName = new QLineEdit;
    m_newName->setMaxLength(60);
    m_newName->setPlaceholderText("e.g. Order intake");
    Surface::field(form, "Name", m_newName);
    m_newDescription = new QPlainTextEdit;
    m_newDescription->setPlaceholderText("What will this blueprint do?");
    m_newDescription->setTabChangesFocus(true);
    m_newDescription->setFixedHeight(88);
    Surface::field(form, "Description (optional)", m_newDescription);
    m_createError = wrapped({}, "blueprintError");
    form->addWidget(m_createError);
    auto *actions = new QHBoxLayout;
    auto *cancel = action("Cancel");
    auto *submit = action("Create blueprint", true);
    submit->setEnabled(false);
    actions->addStretch();
    actions->addWidget(cancel);
    actions->addWidget(submit);
    form->addLayout(actions);
    page->addWidget(form->parentWidget(), 0, Qt::AlignHCenter);
    page->addStretch(3);

    connect(cancel, &Button::clicked, this, &BlueprintPanel::showLibrary);
    connect(submit, &Button::clicked, this, &BlueprintPanel::createBlueprint);
    connect(m_newName, &QLineEdit::returnPressed, this, &BlueprintPanel::createBlueprint);
    connect(m_newName, &QLineEdit::textChanged, submit, [submit](const QString &name) {
        submit->setEnabled(!name.trimmed().isEmpty());
    });
    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), m_createPage);
    escape->setContext(Qt::WidgetWithChildrenShortcut);
    connect(escape, &QShortcut::activated, this, &BlueprintPanel::showLibrary);
}

void BlueprintPanel::showCreatePage() {
    ++m_navigation;
    m_newName->clear();
    m_newDescription->clear();
    m_createError->hide();
    m_pages->setCurrentWidget(m_createPage);
    m_newName->setFocus();
}

void BlueprintPanel::createBlueprint() {
    const auto name = m_newName->text().trimmed();
    if (name.isEmpty()) return;
    const auto id = BlueprintPreview::id();
    QJsonObject graph{{"id", id}, {"revision", m_preview->live() ? 0 : 1}, {"name", name},
        {"description", m_newDescription->toPlainText().trimmed().left(160)}, {"nodes", QJsonArray{}}, {"edges", QJsonArray{}}};
    if (m_preview->live()) graph["dirty"] = true;
    if (!m_preview->saveBlueprint(graph)) {
        m_createError->setText("The blueprint could not be saved.");
        m_createError->show();
        return;
    }
    openBlueprint(id);
}

void BlueprintPanel::editDetails() {
    SettingsDetailDialog dialog(this);
    dialog.setWindowTitle("Edit blueprint details");
    auto *body = new QVBoxLayout(&dialog);
    body->addWidget(label(dialog.windowTitle(), "panelTitle"));
    auto *name = new QLineEdit(m_graph["name"].toString());
    name->setMaxLength(60);
    Surface::field(body, "Name", name);
    auto *description = new QPlainTextEdit(m_graph["description"].toString());
    description->setFixedHeight(76);
    Surface::field(body, "Description", description);
    auto *buttons = dialogButtons(&dialog, body, "Save details");
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (!name->text().trimmed().isEmpty()) dialog.accept();
        else name->setFocus();
    });
    dialog.resize(415, dialog.sizeHint().height());
    name->setFocus();
    if (dialog.exec() != QDialog::Accepted) return;
    auto graph = m_graph;
    graph["name"] = name->text().trimmed();
    graph["description"] = description->toPlainText().trimmed().left(160);
    saveGraph(graph);
}

void BlueprintPanel::deleteBlueprint(const QString &id) {
    const auto graph = findById(m_preview->saved(), id);
    if (id == "order-intake" || graph.isEmpty()) return;
    if (!Ui::confirmRemoval(this, "Delete blueprint", QString("Delete “%1” from the library?").arg(graph["name"].toString()), "Delete")) return;
    if (m_preview->live()) {
        m_preview->removeBlueprint(id, [self = QPointer(this), id](const auto &, const auto &error) {
            if (!self) return;
            if (!error.isEmpty()) Ui::showNotice(self, "Blueprints", error);
            else if (self->m_graph["id"] == id) {
                self->m_graph = {};
                self->m_runId.clear();
                self->showLibrary();
            }
        });
        return;
    }
    if (!m_preview->deleteBlueprint(id)) {
        Ui::showNotice(this, "Blueprints", "The blueprint could not be deleted.");
        return;
    }
    if (m_graph["id"].toString() == id) {
        m_graph = {};
        m_runId.clear();
        showLibrary();
    }
}

void BlueprintPanel::duplicate(const QJsonObject &graph) {
    auto copy = graph;
    copy["id"] = BlueprintPreview::id();
    copy["revision"] = m_preview->live() ? 0 : 1;
    copy["name"] = graph["name"].toString() + " copy";
    if (saveGraph(copy, false)) openBlueprint(copy["id"].toString());
}

bool BlueprintPanel::saveGraph(QJsonObject graph, bool revision) {
    if (m_preview->live()) graph["dirty"] = true;
    else if (revision) graph["revision"] = graph["revision"].toInt() + 1;
    if (!m_preview->saveBlueprint(graph)) {
        Ui::showNotice(this, "Blueprints", "The blueprint could not be saved.");
        return false;
    }
    if (graph["id"] == m_graph["id"]) {
        m_graph = graph;
        refreshEditor();
    }
    return true;
}

void BlueprintPanel::openBlueprint(const QString &id) {
    if (m_preview->live()) {
        const auto nav = ++m_navigation;
        m_preview->getBlueprint(id, false, [self = QPointer(this), nav, id](const QJsonObject &graph, const QString &error) {
            if (!self || nav != self->m_navigation) return;
            if (!error.isEmpty()) { Ui::showNotice(self, "Blueprints", error); return; }
            if (id == "order-intake" && !self->saveGraph(graph, false)) return;
            self->showBlueprint(graph);
        });
        return;
    }
    showBlueprint(m_preview->blueprint(id));
}

void BlueprintPanel::showBlueprint(const QJsonObject &graph) {
    const auto id = graph["id"].toString();
    m_backToChat->hide();
    m_graph = graph;
    if (m_graph.isEmpty()) return;
    const auto nodes = m_graph["nodes"].toArray();
    m_selected = id == "order-intake" && !findById(nodes, "selector").isEmpty()
        ? QString("selector") : nodes.isEmpty() ? QString{} : nodes.first().toObject()["id"].toString();
    m_outputNode.clear();
    m_outputPort.clear();
    const auto recent = m_preview->runs(id);
    m_runId = recent.isEmpty() ? QString{} : recent.first().toObject()["id"].toString();
    m_comparison = false;
    m_tabs->setCurrentIndex(0);
    m_tabs->setTabEnabled(0, true);
    m_tabs->setTabEnabled(2, true);
    m_runSetup->show();
    findChild<Button *>("blueprintEdit")->show();
    findChild<Button *>("blueprintDuplicate")->show();
    findChild<Button *>("blueprintDelete")->setVisible(id != "order-intake");
    showNodePalette(false);
    m_pages->setCurrentWidget(m_workspace);
    refreshEditor(true);
    showTest();
    m_tabs->setCurrentIndex(0);
    if (m_preview->live()) {
        m_preview->refreshHistory(id);
        if (!m_runId.isEmpty()) m_preview->loadRun(m_runId);
    }
}

void BlueprintPanel::publish() {
    if (!m_graph["dirty"].toBool()) return;
    const auto before = m_graph;
    m_workspace->setEnabled(false);
    m_preview->publish(before, [self = QPointer(this), before](const auto &saved, const auto &error) {
        if (!self) return;
        self->m_workspace->setEnabled(true);
        if (!saved.isEmpty() && self->m_graph == before) {
            self->m_graph = saved;
            self->refreshEditor();
            self->refreshRunOptions();
        }
        if (!error.isEmpty()) Ui::showNotice(self, "Save blueprint", error);
    });
}

void BlueprintPanel::reloadRevision() {
    const auto id = m_graph["id"].toString();
    if (!Ui::confirmAction(this, "Reload saved revision", "Replace the edits in this draft with the latest saved revision?", "Reload")) return;
    const auto nav = m_navigation;
    m_preview->getBlueprint(id, true, [self = QPointer(this), nav](const auto &saved, const auto &error) {
        if (!self || nav != self->m_navigation) return;
        if (!error.isEmpty()) { Ui::showNotice(self, "Blueprints", error); return; }
        if (!self->m_preview->saveBlueprint(saved)) { Ui::showNotice(self, "Blueprints", "The local draft could not be updated."); return; }
        self->showBlueprint(saved);
    });
}

void BlueprintPanel::setupEditor() {
    auto *page = new QWidget;
    auto *editor = new QHBoxLayout(page);
    editor->setContentsMargins(0, 9, 0, 0);
    editor->setSpacing(12);
    m_tabs->addTab(page, "Canvas");
    m_canvasHome = new QVBoxLayout;
    editor->addLayout(m_canvasHome, 1);
    auto *panel = new QFrame;
    panel->setObjectName("blueprintCanvasPanel");
    panel->setFrameShape(QFrame::NoFrame);
    m_canvasPanel = panel;
    auto *body = new QVBoxLayout(panel);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(0);
    m_canvasHome->addWidget(panel);
    auto *toolbar = new QHBoxLayout;
    toolbar->setContentsMargins(10, 6, 7, 6);
    toolbar->addWidget(label("Workflow canvas", "headingLabel"));
    toolbar->addStretch();
    m_connect = action("Connect", false, true);
    m_addNode = action("Add node");
    m_addNode->setCheckable(true);
    m_fullscreen = action({}, false, true);
    m_fullscreen->setObjectName("blueprintFullscreen");
    m_fullscreen->setFixedSize(32, 32);
    m_fullscreen->setIconSize(QSize(17, 17));
    toolbar->addWidget(m_connect);
    toolbar->addWidget(m_addNode);
    toolbar->addWidget(m_fullscreen);
    body->addLayout(toolbar);
    connect(m_connect, &Button::clicked, this, [this] { connectPorts(); });
    connect(m_addNode, &Button::clicked, this, [this](bool checked) { showNodePalette(checked); });
    connect(m_fullscreen, &Button::clicked, this, &BlueprintPanel::toggleFullscreen);

    auto *canvasRow = new QHBoxLayout;
    canvasRow->setSpacing(0);
    setupNodePalette();
    canvasRow->addWidget(m_palette);
    m_canvasPages = new QStackedWidget;
    canvasRow->addWidget(m_canvasPages, 1);
    body->addLayout(canvasRow, 1);
    m_canvas = new BlueprintCanvas;
    m_canvasPages->addWidget(m_canvas);
    auto *empty = new QWidget;
    empty->setObjectName("blueprintEmptyCanvas");
    auto *center = new QVBoxLayout(empty);
    center->addStretch();
    auto *startIcon = label({});
    startIcon->setProperty("themeIcon", "message-circle");
    startIcon->setPixmap(Theme::coloredIcon("message-circle", QColor(Theme::textSecondary()), 20).pixmap(20, 20));
    center->addWidget(startIcon, 0, Qt::AlignHCenter);
    center->addWidget(label("Start with a node", "headingLabel"), 0, Qt::AlignHCenter);
    auto *emptyHint = wrapped("Use an Input or Dataset node to supply values.");
    emptyHint->setAlignment(Qt::AlignCenter);
    center->addWidget(emptyHint);
    m_emptyAdd = action("Add node");
    connect(m_emptyAdd, &Button::clicked, this, [this] { showNodePalette(true); });
    center->addWidget(m_emptyAdd, 0, Qt::AlignHCenter);
    center->addStretch();
    m_canvasPages->addWidget(empty);

    auto *footer = new QHBoxLayout;
    footer->setContentsMargins(7, 4, 7, 4);
    m_validation = action({}, false, true);
    m_canvasCount = wrapped({});
    footer->addWidget(m_validation);
    footer->addWidget(m_canvasCount, 1);
    m_gestureHint = wrapped("Drag to pan · Scroll to zoom");
    m_gestureHint->setAlignment(Qt::AlignCenter);
    footer->addWidget(m_gestureHint, 2);
    m_zoomControls = new QWidget;
    auto *zoomRow = new QHBoxLayout(m_zoomControls);
    zoomRow->setContentsMargins(0, 0, 0, 0);
    zoomRow->setSpacing(2);
    auto *minus = action("−", false, true);
    auto *plus = action("+", false, true);
    auto *fit = action("Fit", false, true);
    minus->setAccessibleName("Zoom out");
    plus->setAccessibleName("Zoom in");
    fit->setAccessibleName("Fit canvas");
    m_zoom = label("100%", "blueprintCode");
    zoomRow->addWidget(minus);
    zoomRow->addWidget(m_zoom);
    zoomRow->addWidget(plus);
    zoomRow->addWidget(fit);
    footer->addWidget(m_zoomControls);
    body->addLayout(footer);

    auto *inspector = new QFrame;
    inspector->setObjectName("blueprintSurface");
    inspector->setFixedWidth(243);
    auto *inspectorLayout = new QVBoxLayout(inspector);
    inspectorLayout->setContentsMargins(10, 10, 6, 6);
    auto *inspectorScroll = scroller(&m_inspector);
    m_inspector->setSpacing(6);
    inspectorLayout->addWidget(inspectorScroll);
    editor->addWidget(inspector);

    connect(m_canvas, &BlueprintCanvas::zoomChanged, this, [this](int percent) { m_zoom->setText(QString("%1%").arg(percent)); });
    connect(minus, &Button::clicked, this, [this] { m_canvas->zoomBy(1 / 1.2); });
    connect(plus, &Button::clicked, this, [this] { m_canvas->zoomBy(1.2); });
    connect(fit, &Button::clicked, m_canvas, &BlueprintCanvas::fit);
    connect(m_validation, &Button::clicked, this, [this] {
        if (m_preview->live()) {
            m_preview->validateLive(m_graph, [self = QPointer(this)](const auto &caps, const auto &error) {
                if (!self) return;
                QString detail = error;
                if (detail.isEmpty()) {
                    detail = "The workflow has compatible bindings and connected inputs.";
                    if (caps["device"].isString()) detail += "\nLocal device: " + caps["device"].toString();
                    for (const auto &key : caps["nodes"].toObject().keys()) {
                        const auto node = caps["nodes"].toObject()[key].toObject();
                        detail += QString("\n%1: %2 context tokens%3").arg(key).arg(node["context_tokens"].toInt())
                            .arg(node["reasoning"].toBool() ? " · supports thinking" : "");
                    }
                }
                Ui::showNotice(self, "Workflow validation", detail);
            });
            return;
        }
        const auto issues = m_preview->issues(m_graph);
        Ui::showNotice(this, "Workflow validation", issues.isEmpty()
            ? QString("The workflow has compatible bindings and connected inputs.") : issues.join('\n'));
    });
    connect(m_canvas, &BlueprintCanvas::nodeSelected, this, [this](const QString &id) {
        m_selected = id;
        refreshInspector();
    });
    connect(m_canvas, &BlueprintCanvas::nodeMenuRequested, this, &BlueprintPanel::showNodeMenu);
    connect(m_canvas, &BlueprintCanvas::deleteRequested, this, &BlueprintPanel::removeNode);
    connect(m_canvas, &BlueprintCanvas::nodeMoved, this, [this](const QString &id, const QPoint &position) {
        if (m_preview->live()) {
            auto graph = m_graph;
            auto nodes = graph["nodes"].toArray();
            for (int i = 0; i < nodes.size(); ++i) {
                auto node = nodes.at(i).toObject();
                if (node["id"] != id) continue;
                node["x"] = position.x();
                node["y"] = position.y();
                nodes.replace(i, node);
                graph["nodes"] = nodes;
                if (m_preview->saveBlueprint(graph)) m_graph = graph;
                else {
                    refreshEditor();
                    Ui::showNotice(this, "Blueprints", "The editor layout could not be saved.");
                }
                return;
            }
        }
        auto node = findById(m_graph["nodes"].toArray(), id);
        node["x"] = position.x();
        node["y"] = position.y();
        changeNode(node);
    });
    connect(m_canvas, &BlueprintCanvas::portSelected, this, [this](const QString &node, bool output, const QString &port) {
        if (output) {
            if (m_outputNode == node && m_outputPort == port) {
                m_outputNode.clear();
                m_outputPort.clear();
            } else {
                m_outputNode = node;
                m_outputPort = port;
            }
            refreshInspector();
        } else if (m_outputNode.isEmpty()) connectPorts(node, port);
        else addConnection({{"from", m_outputNode}, {"out", m_outputPort}, {"to", node}, {"in", port}}, m_canvasPanel);
    });
    refreshCanvasActions();
}

void BlueprintPanel::refreshCanvasActions() {
    const bool full = m_fullscreenDialog;
    const QString title = full ? "Exit fullscreen" : "Fullscreen canvas";
    m_fullscreen->setIcon(Theme::coloredIcon(full ? "shrink" : "expand", QColor(Theme::textSecondary()), 16));
    m_fullscreen->setAccessibleName(title);
    m_fullscreen->setToolTip(title);
    for (auto *row : m_palette->findChildren<QPushButton *>("blueprintPaletteItem")) {
        const auto kind = row->property("blueprintKind").toString();
        row->setIcon(Theme::coloredIcon(BlueprintCanvas::nodeIcon(kind), BlueprintCanvas::nodeAccent(kind), 16));
    }
}

void BlueprintPanel::refreshEditor(bool fit) {
    if (m_graph.isEmpty()) return;
    m_title->setText(m_graph["name"].toString());
    const auto revision = m_graph["revision"].toInt();
    const bool dirty = m_graph["dirty"].toBool();
    m_revision->setText(revision ? QString("Revision %1%2").arg(revision).arg(dirty ? " · Draft edits" : "") : "Draft");
    m_publish->setVisible(m_preview->live() && dirty);
    m_publish->setEnabled(m_preview->connected());
    m_reload->setVisible(m_preview->live() && dirty && revision);
    m_reload->setEnabled(m_preview->connected());
    m_description->setText(m_graph["description"].toString("Connect steps and define input and output contracts."));
    if (m_description->text().isEmpty()) m_description->setText("Connect steps and define input and output contracts.");
    const auto fixtures = m_preview->fixtures();
    const auto nodes = m_graph["nodes"].toArray();
    m_canvasPages->setCurrentIndex(nodes.isEmpty() ? 1 : 0);
    m_canvas->setGraph(m_graph, fixtures);
    if (fit) m_canvas->fit();
    m_connect->setVisible(!nodes.isEmpty());
    m_connect->setEnabled(nodes.size() > 1);
    m_addNode->setVisible(!nodes.isEmpty() || m_addNode->isChecked());
    m_zoomControls->setVisible(!nodes.isEmpty());
    m_gestureHint->setVisible(!nodes.isEmpty());
    const auto issues = m_preview->issues(m_graph);
    m_validation->setText(issues.isEmpty() ? (m_preview->live() ? "Check readiness" : "Ready to run")
        : QString("%1 validation issue%2").arg(issues.size()).arg(issues.size() == 1 ? "" : "s"));
    m_validation->setIcon(Theme::coloredIcon(issues.isEmpty() ? "check" : "file-text",
        QColor(issues.isEmpty() ? Theme::statusSuccess() : Theme::statusWarning()), 15));
    const auto connections = m_graph["edges"].toArray().size();
    m_canvasCount->setText(QString("%1 %2 · %3 %4").arg(nodes.size()).arg(nodes.size() == 1 ? "node" : "nodes")
        .arg(connections).arg(connections == 1 ? "link" : "links"));
    // Rebuild the test target menu after structural edits, keeping the selected target.
    const auto target = m_target->currentData().toString();
    const QSignalBlocker blocked(m_target);
    m_target->clear();
    m_target->addItem("Full blueprint", "");
    for (const auto &value : nodes) {
        const auto node = value.toObject();
        m_target->addItem("Node only · " + node["name"].toString(), node["id"].toString());
    }
    m_target->setCurrentIndex(qMax(0, m_target->findData(target)));
    refreshInspector();
    if (m_preview->live()) refreshRunOptions();
}

void BlueprintPanel::refreshInspector() {
    m_canvas->setSelection(m_selected, m_outputNode, m_outputPort);
    clear(m_inspector);
    auto *header = new QHBoxLayout;
    header->addWidget(label("Node inspector", "headingLabel"), 1);
    if (!m_selected.isEmpty()) {
        auto *close = iconAction("window-close", "Clear node selection");
        close->setFixedSize(24, 24);
        header->addWidget(close);
        connect(close, &Button::clicked, this, [this] { m_selected.clear(); refreshInspector(); });
    }
    m_inspector->addLayout(header);
    m_inspector->addWidget(rule());
    const auto nodes = m_graph["nodes"].toArray();
    auto *picker = new ComboBox;
    picker->setAccessibleName("Select node");
    picker->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    picker->addItem("Select a node", "");
    for (const auto &value : nodes) {
        const auto entry = value.toObject();
        picker->addItem(entry["name"].toString(), entry["id"].toString());
    }
    picker->setCurrentIndex(qMax(0, picker->findData(m_selected)));
    if (!nodes.isEmpty()) m_inspector->addWidget(picker);
    else delete picker;
    if (!nodes.isEmpty()) connect(picker, &ComboBox::activated, this, [this, picker] {
        m_selected = picker->currentData().toString();
        m_canvas->revealNode(m_selected);
        refreshInspector();
        QTimer::singleShot(0, this, [this] {
            for (int i = 0; i < m_inspector->count(); ++i) {
                if (auto *widget = m_inspector->itemAt(i)->widget(); widget && widget->accessibleName() == "Select node")
                    widget->setFocus();
            }
        });
    });
    if (!m_outputNode.isEmpty()) m_inspector->addWidget(wrapped("Select a matching input port."));
    const auto node = findById(nodes, m_selected);
    if (node.isEmpty()) {
        m_inspector->addWidget(wrapped("Select a node to see its settings and contracts."));
        m_inspector->addStretch();
        return;
    }
    const auto fixtures = m_preview->fixtures();
    const auto code = BlueprintPreview::operationId(node);
    const auto spec = m_preview->operation(code);
    const auto kind = spec["kind"].toString(node["kind"].toString());
    const auto iconKind = code == "selector" || code == "extractor" ? code : kind;
    auto *identity = new QHBoxLayout;
    identity->setSpacing(9);
    auto *icon = label({});
    icon->setFixedSize(34, 34);
    icon->setAlignment(Qt::AlignCenter);
    icon->setObjectName("blueprintNodeIcon");
    icon->setPixmap(Theme::coloredIcon(BlueprintCanvas::nodeIcon(iconKind), BlueprintCanvas::nodeAccent(iconKind), 19).pixmap(19, 19));
    identity->addWidget(icon);
    auto *names = new QVBoxLayout;
    names->setSpacing(3);
    names->addWidget(wrapped(node["name"].toString(), "blueprintSection"));
    names->addWidget(wrapped(m_preview->nodeKindLabel(kind) + " · " + spec["label"].toString()));
    identity->addLayout(names, 1);
    m_inspector->addLayout(identity);
    m_inspector->addWidget(wrapped(spec["detail"].toString()));
    const auto id = node["id"].toString();
    auto *nodeActions = new QHBoxLayout;
    auto *configure = action("Configure…");
    auto *save = iconAction("folder", "Save to node library");
    nodeActions->addWidget(configure, 1);
    nodeActions->addWidget(save);
    m_inspector->addLayout(nodeActions);
    connect(configure, &Button::clicked, this, [this, id] { configureNode(id); });
    connect(save, &Button::clicked, this, [this, id] { saveNodeTemplate(id); });
    if (m_preview->live() && (kind == "model" || kind == "reasoning")) {
        m_inspector->addWidget(rule());
        m_inspector->addWidget(label("Checkpoint & adapter", "headingLabel"));
        const auto checkpoint = node["model"].toString();
        const auto adapter = node["adapter"].toString();
        m_inspector->addWidget(wrapped(checkpoint.isEmpty() ? "Choose a checkpoint from the local model library."
            : findById(fixtures["models"].toArray(), checkpoint)["name"].toString(checkpoint)));
        m_inspector->addWidget(wrapped(adapter.isEmpty() ? "No adapter" : "Adapter · " + adapter));
        m_inspector->addWidget(wrapped(QString("Output budget · %1 tokens")
            .arg(node["maxTokens"].toInt(kind == "reasoning" ? 1024 : 256))));
        auto *binding = action("Change binding");
        auto *train = action("Training jobs…", false, true);
        m_inspector->addWidget(binding);
        m_inspector->addWidget(train);
        train->setEnabled(m_graph["revision"].toInteger() > 0 && !m_graph["dirty"].toBool());
        if (!train->isEnabled()) train->setToolTip("Save a complete revision before training or binding a job.");
        connect(binding, &Button::clicked, this, &BlueprintPanel::editBinding);
        connect(train, &Button::clicked, this, &BlueprintPanel::liveTraining);
    } else if (m_preview->live() && kind == "dataset") {
        m_inspector->addWidget(rule());
        m_inspector->addWidget(label("Dataset", "headingLabel"));
        const auto dataset = node["dataset"].toString();
        m_inspector->addWidget(wrapped(dataset.isEmpty() ? "Inline rows"
            : findById(m_preview->datasets(), dataset)["name"].toString(dataset)));
        auto *source = action("Choose dataset");
        m_inspector->addWidget(source);
        connect(source, &Button::clicked, this, &BlueprintPanel::editDataset);
    } else if (kind == "model") {
        m_inspector->addWidget(rule());
        m_inspector->addWidget(label("Model & adapter", "headingLabel"));
        auto *base = Surface::card(m_inspector->parentWidget());
        base->parentWidget()->setObjectName("blueprintBinding");
        base->setContentsMargins(9, 6, 9, 6);
        base->setSpacing(3);
        base->addWidget(label("Base model", "cardMeta"));
        base->addWidget(wrapped(named(fixtures, "models", node["model"]), "blueprintBindingValue"));
        m_inspector->addWidget(base->parentWidget());

        auto *adapter = new ComboBox;
        adapter->setAccessibleName("Node adapter");
        adapter->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        adapter->addItem("Select compatible adapter");
        for (const auto &value : m_preview->compatibleAdapters(node["model"].toString(), code)) {
            const auto choice = value.toObject();
            adapter->addItem(choice["name"].toString(), choice["id"].toString());
        }
        adapter->setCurrentIndex(qMax(0, adapter->findData(node["adapter"].toString())));
        adapter->setEnabled(adapter->count() > 1);
        Surface::field(m_inspector, "Node adapter", adapter);
        connect(adapter, &ComboBox::activated, this, [this, adapter, node] {
            const auto chosen = adapter->currentData().toString();
            if (chosen == node["adapter"].toString()) return;
            auto updated = node;
            updated["adapter"] = chosen;
            changeNode(updated);
            for (auto *current : m_inspector->parentWidget()->findChildren<QComboBox *>()) {
                if (current->isVisible() && current->accessibleName() == "Node adapter") current->setFocus();
            }
        });
        const auto model = findById(fixtures["models"].toArray(), node["model"].toString());
        m_inspector->addWidget(label(model["engine"].toString(), "blueprintCode"));
        auto *binding = new QHBoxLayout;
        auto *pickerButton = action("Change binding");
        auto *train = action("Train adapter", false, true);
        binding->addWidget(pickerButton);
        binding->addWidget(train);
        m_inspector->addLayout(binding);
        connect(pickerButton, &Button::clicked, this, &BlueprintPanel::editBinding);
        connect(train, &Button::clicked, this, &BlueprintPanel::saveTrainingRecipe);
        const auto recipe = node["training"].toObject();
        if (!recipe.isEmpty()) m_inspector->addWidget(wrapped("Training recipe saved\n" + recipe["name"].toString()));
    }
    m_inspector->addWidget(rule());
    m_inspector->addWidget(label("Port contracts", "headingLabel"));
    const auto contracts = fixtures["contracts"].toObject();
    for (const auto &side : {QString("in"), QString("out")}) {
        for (const auto &value : node[side].toArray()) {
            const auto port = value.toObject();
            const auto name = m_preview->portLabel(port);
            const auto type = contracts.value(port["type"].toString()).toObject()["label"].toString();
            const bool input = side == "in";
            const auto text = name == type ? name : QString("%1 · %2").arg(name, type);
            auto *edit = new QPushButton(literalButtonText((input ? "In    " : "Out   ") + text + "  ›"));
            edit->setObjectName("blueprintPortContract");
            edit->setAccessibleName(QString("Edit %1 schema for %2").arg(input ? "input" : "output", name));
            edit->setCursor(Qt::PointingHandCursor);
            m_inspector->addWidget(edit);
            connect(edit, &QPushButton::clicked, this, [this, side, port] { editSchema(side, port["id"].toString()); });
        }
    }
    m_inspector->addWidget(rule());
    auto *actions = new QHBoxLayout;
    auto *test = action("Test this node");
    auto *remove = iconAction("trash", "Delete node");
    actions->addWidget(test);
    actions->addStretch();
    actions->addWidget(remove);
    m_inspector->addLayout(actions);
    connect(test, &Button::clicked, this, [this] { showTest(m_selected); });
    connect(remove, &Button::clicked, this, [this, node] { removeNode(node["id"].toString()); });
    m_inspector->addStretch();
}

void BlueprintPanel::changeNode(const QJsonObject &node, bool reconcilePorts) {
    auto graph = m_graph;
    auto nodes = graph["nodes"].toArray();
    for (int i = 0; i < nodes.size(); ++i) {
        if (nodes.at(i).toObject()["id"] != node["id"]) continue;
        nodes.replace(i, node);
        graph["nodes"] = nodes;
        if (reconcilePorts) {
            const auto edges = graph["edges"].toArray();
            QJsonArray retained;
            for (int edgeIndex = 0; edgeIndex < edges.size(); ++edgeIndex) {
                const auto edge = edges.at(edgeIndex).toObject();
                if (edge["from"] == node["id"] || edge["to"] == node["id"]) {
                    auto otherEdges = edges;
                    otherEdges.removeAt(edgeIndex);
                    graph["edges"] = otherEdges;
                    if (!m_preview->connectionError(graph, edge).isEmpty()) continue;
                }
                retained.append(edge);
            }
            const auto removed = edges.size() - retained.size();
            if (removed && !Ui::confirmAction(m_canvasPanel, "Update node",
                    QString("This change will remove %1 incompatible connection(s).").arg(removed), "Save changes")) return;
            graph["edges"] = retained;
            m_outputNode.clear();
            m_outputPort.clear();
        }
        if (!saveGraph(graph)) refreshEditor();
        return;
    }
}

void BlueprintPanel::setupNodePalette() {
    m_palette = new QFrame;
    m_palette->setObjectName("blueprintPalette");
    m_palette->setAccessibleName("Add node panel");
    m_palette->setFixedWidth(194);
    auto *body = new QVBoxLayout(m_palette);
    body->setContentsMargins(10, 8, 10, 8);
    body->setSpacing(5);
    auto *header = new QHBoxLayout;
    header->addWidget(label("Add node", "headingLabel"), 1);
    auto *close = iconAction("window-close", "Close node palette");
    header->addWidget(close);
    body->addLayout(header);
    connect(close, &Button::clicked, this, [this] { showNodePalette(false); });

    body->addWidget(scroller(&m_paletteEntries), 1);
    m_paletteEntries->setSpacing(3);
    refreshNodePalette();
    m_palette->hide();
}

void BlueprintPanel::refreshNodePalette() {
    clear(m_paletteEntries);
    auto row = [this](const QString &title, const QString &detail, const QString &kind, const QString &actionName) {
        auto *button = new QPushButton(literalButtonText(title));
        button->setObjectName("blueprintPaletteItem");
        button->setAccessibleName(actionName);
        button->setToolTip(detail);
        button->setProperty("blueprintKind", kind);
        button->setCursor(Qt::PointingHandCursor);
        button->setIconSize(QSize(16, 16));
        button->setIcon(Theme::coloredIcon(BlueprintCanvas::nodeIcon(kind), BlueprintCanvas::nodeAccent(kind), 16));
        m_paletteEntries->addWidget(button);
        return button;
    };
    for (const auto &value : m_preview->fixtures()["nodeTypes"].toArray()) {
        const auto type = value.toObject();
        const auto kind = type["id"].toString();
        const auto title = type["label"].toString();
        auto *button = row(title, type["detail"].toString(), kind, "Add " + title);
        connect(button, &QPushButton::clicked, this, [this, kind] {
            const auto configured = configureBlueprintNode(*m_preview, m_canvasPanel, kind);
            if (!configured) return;
            auto node = *configured;
            const auto point = m_addAt.value_or(m_canvas->centerInGraph()).toPoint() - QPoint(95, 70);
            node["x"] = point.x();
            node["y"] = point.y();
            addNode(node);
            m_addAt.reset();
        });
    }
    const auto saved = m_preview->nodeLibrary();
    if (!saved.isEmpty()) {
        m_paletteEntries->addSpacing(9);
        m_paletteEntries->addWidget(label("Saved nodes", "headingLabel"));
    }
    for (const auto &value : saved) {
        const auto entry = value.toObject();
        const auto design = entry["node"].toObject();
        const auto spec = m_preview->operation(BlueprintPreview::operationId(design));
        const auto title = entry["name"].toString();
        auto *button = row(title, spec["label"].toString(), spec["kind"].toString(), "Add saved node " + title);
        connect(button, &QPushButton::clicked, this, [this, design] {
            auto node = design;
            const auto point = m_addAt.value_or(m_canvas->centerInGraph()).toPoint() - QPoint(95, 70);
            node["id"] = BlueprintPreview::id();
            node["x"] = point.x();
            node["y"] = point.y();
            addNode(node);
            m_addAt.reset();
        });
        button->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(button, &QWidget::customContextMenuRequested, this, [this, button, entry](const QPoint &position) {
            QMenu menu(m_canvasPanel);
            menu.addAction("Remove from node library…", this, [this, entry] {
                if (!Ui::confirmRemoval(m_canvasPanel, "Remove saved node",
                        QString("Remove “%1” from the node library?").arg(entry["name"].toString()), "Remove")) return;
                if (!m_preview->removeNodeTemplate(entry["id"].toString()))
                    Ui::showNotice(m_canvasPanel, "Saved nodes", "The saved node could not be removed.");
            });
            menu.exec(button->mapToGlobal(position));
        });
    }
    m_paletteEntries->addStretch();
}

void BlueprintPanel::configureNode(const QString &id) {
    const auto node = findById(m_graph["nodes"].toArray(), id);
    if (node.isEmpty()) return;
    const auto spec = m_preview->operation(BlueprintPreview::operationId(node));
    const auto configured = configureBlueprintNode(*m_preview, m_canvasPanel, spec["kind"].toString(), node);
    if (configured && *configured != node) changeNode(*configured, true);
}

void BlueprintPanel::saveNodeTemplate(const QString &id) {
    const auto node = findById(m_graph["nodes"].toArray(), id);
    if (node.isEmpty()) return;
    bool accepted = false;
    const auto name = QInputDialog::getText(m_canvasPanel, "Save to node library", "Name", QLineEdit::Normal,
        node["name"].toString(), &accepted).trimmed().left(60);
    if (!accepted || name.isEmpty()) return;
    if (!m_preview->saveNodeTemplate(name, node)) {
        Ui::showNotice(m_canvasPanel, "Saved nodes", "The node could not be saved.");
        return;
    }
    showNodePalette(true);
}

void BlueprintPanel::showNodePalette(bool open, std::optional<QPointF> position) {
    m_addAt = position;
    m_palette->setVisible(open);
    m_emptyAdd->setVisible(!open);
    m_addNode->setChecked(open);
    m_addNode->setVisible(open || !m_graph["nodes"].toArray().isEmpty());
    if (open) {
        if (auto *first = m_palette->findChild<QPushButton *>("blueprintPaletteItem")) first->setFocus();
    } else if (m_addNode->isVisible()) {
        m_addNode->setFocus();
    }
}

void BlueprintPanel::addNode(const QJsonObject &source) {
    auto graph = m_graph;
    auto nodes = graph["nodes"].toArray();
    auto node = source;
    const QPoint start(node["x"].toInt(), node["y"].toInt());
    // Place the node near the requested location and keep its ports clear of the adjacent cards.
    QPoint position = start;
    for (int slot = 0; slot <= nodes.size(); ++slot) {
        position = start + QPoint((slot % 3) * 219, (slot / 3) * 174);
        bool occupied = false;
        for (const auto &value : nodes) {
            const auto other = value.toObject();
            occupied |= qAbs(other["x"].toInt() - position.x()) < 196 &&
                        qAbs(other["y"].toInt() - position.y()) < 162;
        }
        if (!occupied) break;
    }
    node["x"] = position.x();
    node["y"] = position.y();
    nodes.append(node);
    graph["nodes"] = nodes;
    const auto previous = m_selected;
    m_selected = node["id"].toString();
    if (saveGraph(graph)) m_canvas->revealNode(m_selected);
    else m_selected = previous;
}

void BlueprintPanel::removeNode(const QString &id) {
    const auto node = findById(m_graph["nodes"].toArray(), id);
    if (node.isEmpty()) return;
    if (!Ui::confirmRemoval(m_canvasPanel, "Delete node",
            QString("Delete “%1” and its connections?").arg(node["name"].toString()), "Delete")) return;
    auto graph = m_graph;
    QJsonArray remaining;
    QJsonArray edges;
    for (const auto &value : graph["nodes"].toArray())
        if (value.toObject()["id"].toString() != id) remaining.append(value);
    for (const auto &value : graph["edges"].toArray()) {
        const auto edge = value.toObject();
        if (edge["from"].toString() != id && edge["to"].toString() != id) edges.append(edge);
    }
    graph["nodes"] = remaining;
    graph["edges"] = edges;
    if (!saveGraph(graph)) return;
    if (m_selected == id) m_selected.clear();
    if (m_outputNode == id) {
        m_outputNode.clear();
        m_outputPort.clear();
    }
    refreshInspector();
}

void BlueprintPanel::renameNode(const QString &id) {
    auto node = findById(m_graph["nodes"].toArray(), id);
    if (node.isEmpty()) return;
    bool accepted = false;
    const auto name = QInputDialog::getText(m_canvasPanel, "Rename node", "Name", QLineEdit::Normal,
        node["name"].toString(), &accepted).trimmed().left(60);
    if (!accepted || name.isEmpty() || name == node["name"].toString()) return;
    node["name"] = name;
    changeNode(node);
}

void BlueprintPanel::showNodeMenu(const QString &id, const QPoint &screenPosition, const QPointF &graphPosition) {
    const auto node = findById(m_graph["nodes"].toArray(), id);
    QMenu menu(m_canvasPanel);
    if (node.isEmpty()) {
        menu.addAction("Add node…", this, [this, graphPosition] { showNodePalette(true, graphPosition); });
        menu.addAction("Fit canvas", m_canvas, &BlueprintCanvas::fit);
        menu.exec(screenPosition);
        return;
    }
    m_selected = id;
    refreshInspector();
    menu.addAction("Configure node…", this, [this, id] { configureNode(id); });
    menu.addAction("Rename…", this, [this, id] { renameNode(id); });
    menu.addAction("Duplicate node", this, [this, node] {
        auto copy = node;
        copy["id"] = BlueprintPreview::id();
        copy["name"] = node["name"].toString() + " copy";
        copy["x"] = node["x"].toInt() + 24;
        copy["y"] = node["y"].toInt() + 24;
        addNode(copy);
    });
    menu.addAction("Save to node library…", this, [this, id] { saveNodeTemplate(id); });
    const auto kind = BlueprintPreview::operationId(node);
    if (kind == "selector" || kind == "extractor") {
        menu.addSeparator();
        menu.addAction("Change binding…", this, &BlueprintPanel::editBinding);
        menu.addAction("Train adapter…", this, &BlueprintPanel::saveTrainingRecipe);
    } else if (m_preview->live() && (kind == "generate" || kind == "reason")) {
        menu.addSeparator();
        menu.addAction("Change binding…", this, &BlueprintPanel::editBinding);
    } else if (m_preview->live() && kind == "dataset-rows") {
        menu.addSeparator();
        menu.addAction("Choose dataset…", this, &BlueprintPanel::editDataset);
    }
    menu.addSeparator();
    menu.addAction("Test this node", this, [this, id] {
        if (m_fullscreenDialog) m_fullscreenDialog->reject();
        showTest(id);
    });
    menu.addAction("Delete node…", this, [this, id] { removeNode(id); });
    menu.exec(screenPosition);
}

void BlueprintPanel::connectPorts(const QString &inputNode, const QString &inputPort) {
    SettingsDetailDialog dialog(m_canvasPanel);
    dialog.setWindowTitle("Connect ports");
    auto *body = new QVBoxLayout(&dialog);
    titleBar(body, "Connect ports");
    const auto contracts = m_preview->fixtures()["contracts"].toObject();
    auto *out = new ComboBox;
    auto *in = new ComboBox;
    auto addPorts = [&](ComboBox *combo, const QString &side, const QString &selectedNode, const QString &selectedPort) {
        combo->addItem(side == "out" ? "Select output" : "Select input");
        for (const auto &value : m_graph["nodes"].toArray()) {
            const auto node = value.toObject();
            for (const auto &portValue : node[side].toArray()) {
                const auto port = portValue.toObject();
                const auto nodeId = node["id"].toString();
                const auto portId = port["id"].toString();
                const auto type = contracts.value(port["type"].toString()).toObject()["label"].toString();
                const auto name = m_preview->portLabel(port);
                const auto portDescription = name == type ? name : QString("%1 (%2)").arg(name, type);
                combo->addItem(node["name"].toString() + " · " + portDescription, QStringList{nodeId, portId});
                if (nodeId == selectedNode && portId == selectedPort) combo->setCurrentIndex(combo->count() - 1);
            }
        }
    };
    addPorts(out, "out", m_outputNode, m_outputPort);
    addPorts(in, "in", inputNode, inputPort);
    Surface::field(body, "Output port", out);
    Surface::field(body, "Input port", in);
    auto *message = wrapped({}, "blueprintError");
    body->addWidget(message);

    auto *existing = new QListWidget;
    existing->setAccessibleName("Existing connections");
    auto *remove = action("Remove selected connection", false, true);
    auto refresh = [&] {
        existing->clear();
        for (const auto &value : m_graph["edges"].toArray()) {
            const auto edge = value.toObject();
            const auto nodes = m_graph["nodes"].toArray();
            const auto source = findById(nodes, edge["from"].toString());
            const auto target = findById(nodes, edge["to"].toString());
            const auto sourcePort = findById(source["out"].toArray(), edge["out"].toString());
            const auto targetPort = findById(target["in"].toArray(), edge["in"].toString());
            existing->addItem(QString("%1 · %2 → %3 · %4").arg(source["name"].toString(),
                m_preview->portLabel(sourcePort), target["name"].toString(), m_preview->portLabel(targetPort)));
        }
        remove->setEnabled(false);
    };
    refresh();
    body->addWidget(label("Existing connections", "headingLabel"));
    body->addWidget(existing, 1);
    body->addWidget(remove, 0, Qt::AlignLeft);
    connect(existing, &QListWidget::currentRowChanged, &dialog, [remove](int row) { remove->setEnabled(row >= 0); });
    connect(remove, &Button::clicked, &dialog, [&] {
        if (existing->currentRow() < 0) return;
        auto graph = m_graph;
        auto edges = graph["edges"].toArray();
        edges.removeAt(existing->currentRow());
        graph["edges"] = edges;
        if (saveGraph(graph)) refresh();
    });
    auto *buttons = dialogButtons(&dialog, body, "Connect");
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const auto a = out->currentData().toStringList();
        const auto b = in->currentData().toStringList();
        const QJsonObject edge{{"from", a.value(0)}, {"out", a.value(1)}, {"to", b.value(0)}, {"in", b.value(1)}};
        const auto error = m_preview->connectionError(m_graph, edge);
        message->setText(error);
        if (error.isEmpty() && addConnection(edge, &dialog)) dialog.accept();
    });
    dialog.resize(465, 385);
    dialog.exec();
}

bool BlueprintPanel::addConnection(const QJsonObject &edge, QWidget *parent) {
    const auto error = m_preview->connectionError(m_graph, edge);
    if (!error.isEmpty()) {
        Ui::showNotice(parent, "Connect ports", error);
        return false;
    }
    auto graph = m_graph;
    auto edges = graph["edges"].toArray();
    edges.append(edge);
    graph["edges"] = edges;
    m_outputNode.clear();
    m_outputPort.clear();
    return saveGraph(graph);
}

void BlueprintPanel::editBinding() {
    if (m_preview->live()) { editLiveBinding(); return; }
    auto node = findById(m_graph["nodes"].toArray(), m_selected);
    SettingsDetailDialog dialog(m_canvasPanel);
    dialog.setWindowTitle("Model & adapter");
    auto *body = new QVBoxLayout(&dialog);
    titleBar(body, dialog.windowTitle());
    body->addWidget(wrapped("Choose a local binding for " + node["name"].toString() + "."));
    const auto fixtures = m_preview->fixtures();
    auto *models = new ComboBox;
    auto *adapters = new ComboBox;
    for (const auto &value : fixtures["models"].toArray()) {
        const auto model = value.toObject();
        models->addItem(model["name"].toString() + " · " + model["engine"].toString(), model["id"].toString());
    }
    models->setCurrentIndex(qMax(0, models->findData(node["model"].toString())));
    Surface::field(body, "Base checkpoint", models);
    Surface::field(body, "Reuse a LoRA adapter", adapters);
    auto *note = wrapped({});
    body->addWidget(note);
    auto refresh = [&] {
        const auto model = findById(fixtures["models"].toArray(), models->currentData().toString());
        adapters->clear();
        adapters->addItem("Select compatible adapter");
        const bool supported = model["supported"].toBool();
        for (const auto &value : m_preview->compatibleAdapters(model["id"].toString(), BlueprintPreview::operationId(node))) {
            const auto adapter = value.toObject();
            adapters->addItem(adapter["name"].toString(), adapter["id"].toString());
        }
        adapters->setCurrentIndex(qMax(0, adapters->findData(node["adapter"].toString())));
        adapters->setEnabled(supported);
        note->setText(!supported ? "This engine cannot switch PEFT adapters between blueprint nodes."
            : adapters->count() == 1 ? "No adapters match this checkpoint and node type."
            : "Adapters must match the selected checkpoint and node type.");
    };
    refresh();
    connect(models, &ComboBox::currentIndexChanged, &dialog, refresh);
    auto *buttons = dialogButtons(&dialog, body, "Save binding");
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (!adapters->isEnabled() || adapters->currentIndex() < 1) {
            note->setText("Choose a Torch / PEFT checkpoint and a compatible adapter.");
            return;
        }
        dialog.accept();
    });
    dialog.resize(450, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted) return;
    node["model"] = models->currentData().toString();
    node["adapter"] = adapters->currentData().toString();
    changeNode(node);
}

void BlueprintPanel::editLiveBinding() {
    auto node = findById(m_graph["nodes"].toArray(), m_selected);
    if (node.isEmpty()) return;
    const bool reasoning = node["kind"] == "reasoning";
    SettingsDetailDialog dialog(this);
    dialog.setWindowTitle(reasoning ? "Reasoning checkpoint" : "Model & adapter");
    auto *body = new QVBoxLayout(&dialog);
    titleBar(body, dialog.windowTitle());
    auto *models = new ComboBox;
    auto *adapters = new ComboBox;
    for (auto *combo : {models, adapters}) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(22);
    }
    Surface::field(body, "Base checkpoint", models);
    Surface::field(body, "Compatible LoRA adapter", adapters);
    auto *note = wrapped({});
    body->addWidget(note);
    auto *budget = new QSpinBox;
    budget->setRange(1, 32768);
    budget->setValue(node["maxTokens"].toInt(reasoning ? 1024 : 256));
    Surface::field(body, "Maximum output tokens", budget);
    auto *response = new ComboBox;
    response->addItem("Typed JSON", "json");
    response->addItem("Plain text", "text");
    response->setCurrentIndex(node["response"] == "text" ? 1 : 0);
    Surface::field(body, "Response format", response);
    auto *prompt = new QPlainTextEdit(node["prompt"].toString("Return only the requested JSON value."));
    prompt->setTabChangesFocus(true);
    prompt->setFixedHeight(100);
    Surface::field(body, "System prompt", prompt);
    auto *buttons = dialogButtons(&dialog, body, "Save binding");
    auto *save = buttons->buttons().last();
    for (auto *button : buttons->buttons())
        if (buttons->buttonRole(button) == QDialogButtonBox::AcceptRole) save = button;
    auto refresh = [&] {
        const auto model = models->currentData().toString();
        adapters->clear();
        adapters->addItem("No adapter", "");
        adapters->setEnabled(false);
        save->setEnabled(false);
        if (model.isEmpty()) {
            note->setText("Download a Transformers checkpoint in the model library, then select it here.");
            return;
        }
        note->setText("Checking this checkpoint…");
        m_preview->checkpoint(model, [guard = QPointer(&dialog), models, adapters, note, budget, save, reasoning, node, model]
            (const auto &info, const auto &error) {
            if (!guard || models->currentData().toString() != model) return;
            if (!error.isEmpty()) { note->setText(error); return; }
            const bool supported = info["reasoning"].toBool();
            if (reasoning && !supported) { note->setText("This checkpoint does not support explicit thinking."); return; }
            const int context = info["context_tokens"].toInt();
            note->setText(QString("%1 context tokens%2").arg(context).arg(supported ? " · Explicit thinking available" : ""));
            budget->setMaximum(qBound(1, context - 1, 32768));
            for (const auto &value : info["adapters"].toArray()) {
                const auto adapter = value.toObject();
                adapters->addItem(adapter["name"].toString(), adapter["id"].toString());
            }
            adapters->setCurrentIndex(qMax(0, adapters->findData(node["adapter"].toString())));
            adapters->setEnabled(true);
            save->setEnabled(true);
        });
    };
    const auto options = [&] {
        const auto selected = models->currentData().toString();
        QSignalBlocker blocker(models);
        models->clear();
        models->addItem("Choose a checkpoint", "");
        const auto entries = m_preview->fixtures()["models"].toArray();
        for (const auto &value : entries) {
            const auto entry = value.toObject();
            models->addItem(entry["name"].toString(), entry["id"].toString());
        }
        models->setCurrentIndex(qMax(0, models->findData(selected.isEmpty() ? node["model"].toString() : selected)));
        refresh();
    };
    options();
    m_preview->refreshOptions();
    connect(m_preview, &BlueprintPreview::optionsChanged, &dialog, options);
    connect(models, &ComboBox::currentIndexChanged, &dialog, refresh);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (prompt->toPlainText().size() > 16000) { note->setText("Use at most 16,000 characters for the system prompt."); return; }
        if (response->currentData() == "text") {
            const auto ports = node["out"].toArray();
            if (ports.size() != 1 || ports.first().toObject()["schema"].toObject()["type"] != "string") {
                note->setText("Plain text requires one string output port.");
                return;
            }
        }
        dialog.accept();
    });
    dialog.resize(460, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted) return;
    node["model"] = models->currentData().toString();
    node["adapter"] = adapters->currentData().toString();
    node["maxTokens"] = budget->value();
    node["response"] = response->currentData().toString();
    node["prompt"] = prompt->toPlainText();
    changeNode(node);
}

void BlueprintPanel::editDataset() {
    auto node = findById(m_graph["nodes"].toArray(), m_selected);
    SettingsDetailDialog dialog(this);
    dialog.setWindowTitle("Dataset source");
    auto *body = new QVBoxLayout(&dialog);
    titleBar(body, dialog.windowTitle());
    auto *datasets = new ComboBox;
    datasets->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    datasets->setMinimumContentsLength(22);
    auto populate = [&] {
        const auto current = datasets->count() ? datasets->currentData().toString() : node["dataset"].toString();
        datasets->clear();
        datasets->addItem("Inline rows", "");
        for (const auto &value : m_preview->datasets()) {
            const auto item = value.toObject();
            datasets->addItem(QString("%1 · %2 rows").arg(item["name"].toString()).arg(item["rows"].toInt()), item["id"].toString());
        }
        if (!current.isEmpty() && datasets->findData(current) < 0) datasets->addItem("Missing dataset · " + current, current);
        datasets->setCurrentIndex(qMax(0, datasets->findData(current)));
    };
    populate();
    connect(m_preview, &BlueprintPreview::optionsChanged, &dialog, populate);
    m_preview->refreshOptions();
    Surface::field(body, "Source", datasets);
    auto *rows = new QPlainTextEdit(node["setting"].toString("[]"));
    rows->setTabChangesFocus(true);
    auto *field = new QWidget;
    auto *rowsBody = new QVBoxLayout(field);
    rowsBody->setContentsMargins(0, 0, 0, 0);
    Surface::field(rowsBody, "Inline rows (JSON array)", rows);
    body->addWidget(field);
    const auto enabled = [datasets, field] { field->setVisible(datasets->currentData().toString().isEmpty()); };
    connect(datasets, &ComboBox::currentIndexChanged, &dialog, enabled);
    enabled();
    body->addWidget(wrapped("Add local datasets in Training. Set the row type under the Rows port’s JSON Schema."));
    auto *note = wrapped({}, "blueprintError");
    body->addWidget(note);
    auto *buttons = dialogButtons(&dialog, body, "Save dataset");
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        node["dataset"] = datasets->currentData().toString();
        node["setting"] = rows->toPlainText();
        const auto error = m_preview->settingError(node);
        if (error.isEmpty()) dialog.accept();
        else note->setText(error);
    });
    dialog.resize(470, 340);
    if (dialog.exec() == QDialog::Accepted) changeNode(node);
}

void BlueprintPanel::liveTraining() {
    const auto graph = m_graph;
    const auto node = m_selected;
    if (graph["dirty"].toBool() || graph["revision"].toInteger() < 1) return;
    SettingsDetailDialog dialog(this);
    dialog.setWindowTitle("Train or bind an adapter");
    auto *body = new QVBoxLayout(&dialog);
    titleBar(body, dialog.windowTitle());
    auto *jobs = new ComboBox;
    jobs->setAccessibleName("Training jobs for this node");
    jobs->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    body->addWidget(jobs);
    auto *bind = action("Bind completed adapter");
    bind->setEnabled(false);
    body->addWidget(bind, 0, Qt::AlignRight);
    body->addWidget(rule());
    auto *name = new QLineEdit;
    name->setPlaceholderText("Use an automatic adapter name");
    Surface::field(body, "Adapter name (optional)", name);
    auto *examples = new QPlainTextEdit;
    examples->setTabChangesFocus(true);
    examples->setPlaceholderText("[{\"inputs\": {\"port\": \"value\"}, \"outputs\": {\"port\": \"answer\"}}]");
    Surface::field(body, "Typed examples (JSON array)", examples);
    const auto sample = m_preview->orderExamples(node);
    if (!sample.isEmpty()) {
        auto *use = action("Use order training examples", false, true);
        body->addWidget(use, 0, Qt::AlignLeft);
        connect(use, &Button::clicked, examples, [examples, sample] { examples->setPlainText(BlueprintPreview::jsonText(sample)); });
    }
    auto *note = wrapped("Provide examples matching the named input and output ports. Follow queued jobs in Training.");
    body->addWidget(note);
    auto *buttons = dialogButtons(&dialog, body, "Queue training");
    const auto pending = [buttons, bind](bool busy) {
        buttons->setEnabled(!busy);
        bind->setProperty("busy", busy);
        if (busy) bind->setEnabled(false);
    };
    auto *timer = new QTimer(&dialog);
    auto refresh = [this, graph, node, jobs, bind, note, guard = QPointer(&dialog)] {
        m_preview->jobs(graph["id"].toString(), node, [jobs, bind, note, guard](const auto &info, const auto &error) {
            if (!guard) return;
            if (!error.isEmpty()) { note->setText(error); return; }
            auto display = QStringList{};
            for (const auto &value : info["jobs"].toArray()) {
                const auto job = value.toObject();
                display << job["id"].toString() + ":" + job["status"].toString() + ":" + job["message"].toString();
            }
            if (jobs->count() && jobs->property("jobList").toStringList() == display) return;
            jobs->setProperty("jobList", display);
            const auto selected = jobs->currentData().toString();
            jobs->clear();
            jobs->addItem("Choose a node training job", "");
            for (const auto &value : info["jobs"].toArray()) {
                const auto job = value.toObject();
                const auto status = job["status"].toString();
                jobs->addItem(job["name"].toString() + " · " + status, job["id"].toString());
                jobs->setItemData(jobs->count() - 1, status == "done" && job["adapter_id"].isString(), Qt::UserRole + 1);
                jobs->setItemData(jobs->count() - 1, job["error"].toString(job["message"].toString()), Qt::ToolTipRole);
            }
            jobs->setCurrentIndex(qMax(0, jobs->findData(selected)));
        });
    };
    connect(jobs, &ComboBox::currentIndexChanged, &dialog, [jobs, bind] {
        bind->setEnabled(!bind->property("busy").toBool() && jobs->currentData(Qt::UserRole + 1).toBool());
    });
    connect(timer, &QTimer::timeout, &dialog, refresh);
    timer->start(2500);
    refresh();
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&, guard = QPointer(&dialog)] {
        const auto parsed = QJsonDocument::fromJson(examples->toPlainText().toUtf8());
        if (!parsed.isArray() || parsed.array().isEmpty()) { note->setText("Enter at least one typed example as a JSON array."); return; }
        pending(true);
        m_preview->train(graph, node, parsed.array(), QJsonObject{{"name", name->text().trimmed()}, {"target_modules", "all-linear"}},
            [guard, note, jobs, bind, refresh, pending](const auto &job, const auto &error) {
                if (!guard) return;
                pending(false);
                bind->setEnabled(jobs->currentData(Qt::UserRole + 1).toBool());
                note->setText(error.isEmpty() ? "Queued " + job["name"].toString() + ". Select the job when it is done." : error);
                refresh();
            });
    });
    connect(bind, &Button::clicked, &dialog, [&, guard = QPointer(&dialog)] {
        pending(true);
        m_preview->bindTraining(graph, node, jobs->currentData().toString(),
            [guard, self = QPointer(this), graph, note, jobs, bind, pending](const auto &saved, const auto &error) {
                if (!self) return;
                if (!saved.isEmpty() && self->m_graph == graph) {
                    self->m_graph = saved;
                    self->refreshEditor();
                }
                if (!guard) return;
                if (error.isEmpty()) guard->accept();
                else {
                    pending(false);
                    bind->setEnabled(jobs->currentData(Qt::UserRole + 1).toBool());
                    note->setText(error);
                }
            });
    });
    dialog.resize(560, 465);
    dialog.exec();
}

void BlueprintPanel::saveTrainingRecipe() {
    auto node = findById(m_graph["nodes"].toArray(), m_selected);
    const auto existing = node["training"].toObject();
    SettingsDetailDialog dialog(m_canvasPanel);
    dialog.setWindowTitle("Train an adapter");
    auto *body = new QVBoxLayout(&dialog);
    titleBar(body, dialog.windowTitle());
    auto *name = new QLineEdit(existing["name"].toString(BlueprintPreview::operationId(node) == "selector" ? "new-order-selector" : "new-order-fields"));
    name->setMaxLength(48);
    auto *dataset = new ComboBox;
    dataset->addItems({"Order intake examples · fixture", "Small smoke-test dataset · fixture"});
    if (!existing.isEmpty()) dataset->setCurrentText(existing["dataset"].toString());
    Surface::field(body, "Adapter name", name);
    Surface::field(body, "Dataset", dataset);
    body->addWidget(wrapped("Save a fixture training recipe using this node’s checkpoint and port contracts."));
    auto *buttons = dialogButtons(&dialog, body, "Save recipe");
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        if (!name->text().trimmed().isEmpty()) dialog.accept();
        else name->setFocus();
    });
    dialog.resize(440, dialog.sizeHint().height());
    if (dialog.exec() != QDialog::Accepted) return;
    node["training"] = QJsonObject{{"name", name->text().trimmed()}, {"dataset", dataset->currentText()}, {"model", node["model"]}};
    changeNode(node);
}

void BlueprintPanel::editSchema(const QString &side, const QString &portId) {
    auto node = findById(m_graph["nodes"].toArray(), m_selected);
    auto ports = node[side].toArray();
    SettingsDetailDialog dialog(this);
    dialog.setWindowTitle(side == "in" ? "Edit input schema" : "Edit output schema");
    auto *body = new QVBoxLayout(&dialog);
    titleBar(body, dialog.windowTitle());
    const auto port = findById(ports, portId);
    auto *editor = new QPlainTextEdit(BlueprintPreview::jsonText(port["schema"]));
    editor->setObjectName("blueprintCode");
    Surface::field(body, "JSON Schema", editor);
    auto *message = wrapped({}, "blueprintError");
    body->addWidget(message);
    QJsonObject schema;
    auto *buttons = dialogButtons(&dialog, body, "Save schema");
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        QJsonParseError error;
        schema = QJsonDocument::fromJson(editor->toPlainText().toUtf8(), &error).object();
        if (error.error != QJsonParseError::NoError) message->setText("Enter valid JSON.");
        else if (!BlueprintPreview::supportedSchema(schema))
            message->setText("Use a supported type: string, integer, number, boolean, object, array, or null.");
        else if (schema["type"] != port["schema"].toObject()["type"])
            message->setText("Change the port type in Configure node.");
        else dialog.accept();
    });
    dialog.resize(495, 385);
    if (dialog.exec() != QDialog::Accepted) return;
    for (int i = 0; i < ports.size(); ++i) {
        auto edited = ports.at(i).toObject();
        if (edited["id"].toString() != portId) continue;
        edited["schema"] = schema;
        ports.replace(i, edited);
        break;
    }
    node[side] = ports;
    changeNode(node);
}

void BlueprintPanel::toggleFullscreen() {
    if (m_fullscreenDialog) {
        m_fullscreenDialog->reject();
        return;
    }
    auto *dialog = new QDialog(this, Qt::Dialog | Qt::FramelessWindowHint);
    m_fullscreenDialog = dialog;
    dialog->setWindowTitle("Workflow canvas");
    dialog->setWindowModality(Qt::WindowModal);
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(0, 0, 0, 0);
    m_canvasHome->removeWidget(m_canvasPanel);
    layout->addWidget(m_canvasPanel);
    connect(dialog, &QDialog::finished, this, &BlueprintPanel::leaveFullscreen);
    refreshCanvasActions();
    dialog->showFullScreen();
    m_fullscreen->setFocus();
}

void BlueprintPanel::leaveFullscreen() {
    auto *dialog = std::exchange(m_fullscreenDialog, nullptr);
    if (!dialog) return;
    dialog->layout()->removeWidget(m_canvasPanel);
    m_canvasHome->addWidget(m_canvasPanel);
    m_canvasPanel->show();
    dialog->deleteLater();
    refreshCanvasActions();
    m_fullscreen->setFocus();
}

void BlueprintPanel::setupRun() {
    auto *page = new QWidget;
    auto *layout = new QHBoxLayout(page);
    layout->setContentsMargins(0, 9, 0, 0);
    layout->setSpacing(16);
    m_traceScroll = scroller(&m_trace);
    m_traceScroll->setObjectName("blueprintRunDetails");
    layout->addWidget(m_traceScroll, 1);
    QVBoxLayout *setup;
    m_runSetup = scroller(&setup);
    m_runSetup->setFixedWidth(278);
    layout->addWidget(m_runSetup);
    auto *card = Surface::card(m_runSetup, "Run setup");
    card->parentWidget()->setObjectName("blueprintSurface");
    setup->addWidget(card->parentWidget());
    setup->addStretch();
    m_target = new ComboBox;
    m_sample = new ComboBox;
    m_scenario = new ComboBox;
    for (auto *combo : {m_target, m_sample, m_scenario}) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(16);
        connect(combo, &ComboBox::currentTextChanged, combo, &QWidget::setToolTip);
    }
    m_sample->addItem("Edited input");
    const auto fixtures = m_preview->fixtures();
    for (const auto &value : fixtures["samples"].toArray()) {
        const auto sample = value.toObject();
        m_sample->addItem(sample["label"].toString(), sampleText(sample));
    }
    for (const auto &value : fixtures["scenarios"].toArray()) {
        const auto scenario = value.toObject();
        m_scenario->addItem(scenario["label"].toString(), scenario["id"].toString());
    }
    Surface::field(card, "Run target", m_target);
    auto makeField = [card](const QString &name, QWidget *editor) {
        auto *field = new QWidget;
        auto *layout = new QVBoxLayout(field);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(5);
        Surface::field(layout, name, editor);
        card->addWidget(field);
        return field;
    };
    m_sampleField = makeField("Sample input", m_sample);
    m_input = new QPlainTextEdit;
    m_input->setAccessibleName("Run input");
    m_input->setFixedHeight(112);
    m_input->setTabChangesFocus(true);
    card->addWidget(m_input);
    m_scenarioField = makeField("Fixture scenario", m_scenario);
    m_fixtureNote = wrapped({});
    m_runError = wrapped({}, "blueprintError");
    m_runError->hide();
    card->addWidget(m_fixtureNote);
    card->addWidget(m_runError);
    m_start = action("Start run", true);
    m_start->setObjectName("blueprintStartRun");
    card->addWidget(m_start, 0, Qt::AlignLeft);
    connect(m_target, &ComboBox::currentIndexChanged, this, &BlueprintPanel::refreshRunOptions);
    connect(m_sample, &ComboBox::activated, this, [this] {
        if (m_sample->currentIndex() > 0) m_input->setPlainText(m_sample->currentData().toString());
        refreshRunOptions();
    });
    connect(m_input, &QPlainTextEdit::textChanged, this, [this] {
        const QSignalBlocker blocker(m_sample);
        m_sample->setCurrentIndex(qMax(0, m_sample->findData(m_input->toPlainText().trimmed())));
        refreshRunOptions();
    });
    connect(m_start, &Button::clicked, this, &BlueprintPanel::startRun);
    m_tabs->addTab(page, "Test && run");
}

void BlueprintPanel::refreshRunOptions() {
    const auto target = m_target->currentData().toString();
    const bool nodeOnly = !target.isEmpty();
    if (m_preview->live()) {
        const auto contracts = BlueprintPreview::inputContracts(m_graph, target);
        const auto samples = m_preview->orderSamples();
        const bool order = target.isEmpty() && contracts.size() == 1 && contracts.contains("message")
            && !findById(m_graph["nodes"].toArray(), "price").isEmpty() && !samples.isEmpty();
        if (order && m_sample->count() != samples.size() + 1) {
            const QSignalBlocker blocked(m_sample);
            m_sample->clear();
            m_sample->addItem("Edited input");
            for (int i = 0; i < samples.size(); ++i) {
                const auto inputs = samples.at(i).toObject()["request"].toObject()["inputs"].toObject();
                m_sample->addItem(QString("Order example %1").arg(i + 1), inputs["message"].toString());
            }
        }
        m_sampleField->setVisible(order);
        m_scenarioField->hide();
        m_input->setVisible(!contracts.isEmpty());
        m_input->setPlaceholderText(inputHint(contracts, nodeOnly));
        auto hint = inputHint(contracts, nodeOnly);
        const bool draft = m_graph["dirty"].toBool() || !m_graph["revision"].toInteger();
        if (draft && !nodeOnly) hint = "Save a revision before starting a full run.\n" + hint;
        if (!m_preview->connected()) hint = "Waiting for the local service.\n" + hint;
        m_fixtureNote->setText(hint);
        m_fixtureNote->show();
        m_start->setText(nodeOnly ? "Test node" : "Start run");
        m_start->setEnabled(m_preview->connected() && !m_starting && (nodeOnly || !draft));
        return;
    }
    const bool orderSample = m_preview->usesOrderSample(m_graph, target);
    m_start->setText(nodeOnly ? "Test node" : "Start run");
    m_sampleField->setVisible(orderSample);
    m_scenarioField->setVisible(orderSample);
    if (orderSample) {
        m_input->show();
        m_input->setPlaceholderText("Enter an order message.");
        const QString note = nodeOnly ? "The selected step receives sample values for each input port."
            : "Approval fixtures add a tool request after pricing.";
        m_fixtureNote->setText(m_sample->currentIndex() == 0
            ? note + " Edited input uses the starter sample’s adapter responses." : note);
        m_fixtureNote->show();
        return;
    }
    bool acceptsInput = false;
    for (const auto &value : m_graph["nodes"].toArray()) {
        const auto node = value.toObject();
        const auto code = BlueprintPreview::operationId(node);
        if (!nodeOnly) acceptsInput |= code == "input";
        else if (node["id"].toString() == target)
            acceptsInput = code == "input" || !node["in"].toArray().isEmpty();
    }
    m_input->setVisible(acceptsInput);
    m_input->setPlaceholderText("Enter text or JSON for the input type.");
    m_fixtureNote->setText("Leave the input empty to use a sample value.");
    m_fixtureNote->setVisible(nodeOnly && acceptsInput && BlueprintPreview::operationId(
        findById(m_graph["nodes"].toArray(), target)) != "input");
}

void BlueprintPanel::showTest(const QString &node) {
    if (m_graph.isEmpty()) return;
    m_tabs->setCurrentIndex(1);
    m_runError->hide();
    m_target->setCurrentIndex(qMax(0, m_target->findData(node)));
    m_scenario->setCurrentIndex(0);
    m_input->setPlainText(m_preview->usesOrderSample(m_graph, node) ? m_sample->itemData(1).toString() : QString{});
    refreshRunOptions();
    refreshRun();
}

void BlueprintPanel::startRun() {
    QString error;
    const auto target = m_target->currentData().toString();
    if (m_preview->live()) {
        if (!m_start->isEnabled()) return;
        const auto contracts = BlueprintPreview::inputContracts(m_graph, target);
        const auto input = collectInput(contracts, m_input->toPlainText(), &error);
        m_runError->setText(error);
        m_runError->setVisible(!error.isEmpty());
        if (!error.isEmpty()) return;
        m_starting = true;
        m_start->setEnabled(false);
        const auto nav = m_navigation;
        m_preview->runLive(m_graph, input, target, false, false,
            [self = QPointer(this), nav](const auto &run, const auto &error) {
                if (!self) return;
                self->m_starting = false;
                self->refreshRunOptions();
                if (nav != self->m_navigation) return;
                self->m_runError->setText(error);
                self->m_runError->setVisible(!error.isEmpty());
                if (run.isEmpty()) return;
                self->m_runId = run["id"].toString();
                self->m_backToChat->hide();
                self->refreshRun();
                self->m_traceScroll->verticalScrollBar()->setValue(0);
            });
        return;
    }
    const auto scenario = m_preview->usesOrderSample(m_graph, target) ? m_scenario->currentData().toString() : QString("normal");
    const auto id = m_preview->start(m_graph, m_input->isHidden() ? QString{} : m_input->toPlainText(),
        target, scenario, "Blueprints", &error);
    m_runError->setText(error);
    m_runError->setVisible(!error.isEmpty());
    if (id.isEmpty()) return;
    m_runId = id;
    m_comparison = false;
    refreshRun();
    m_traceScroll->verticalScrollBar()->setValue(0);
}

void BlueprintPanel::openRun(const QString &id) {
    if (m_preview->live()) {
        const auto nav = ++m_navigation;
        const auto display = [self = QPointer(this), nav, id](const auto &run, const auto &error) {
            if (!self || nav != self->m_navigation) return;
            if (!error.isEmpty() && run.isEmpty()) { Ui::showNotice(self, "Inspect run", error); return; }
            self->showRun(id, run);
        };
        const auto cached = m_preview->run(id);
        if (!cached.isEmpty()) display(cached, QString{});
        m_preview->loadRun(id, [display, cached](const auto &run, const auto &error) {
            if (cached.isEmpty()) display(run, error);
        });
        return;
    }
    showRun(id, m_preview->run(id));
}

void BlueprintPanel::showRun(const QString &id, const QJsonObject &run) {
    if (run.isEmpty()) return;
    const auto graphId = run["blueprintId"].toString();
    if (m_preview->live() && m_pages->currentWidget() == m_workspace && !graphId.isEmpty() && m_graph["id"] == graphId) {
        // The current editor stays available; the trace always uses its frozen snapshot.
    } else if (!m_preview->live() && !graphId.isEmpty() && !m_preview->blueprint(graphId).isEmpty()) {
        openBlueprint(graphId);
        m_target->setCurrentIndex(qMax(0, m_target->findData(run["target"].toString())));
        m_scenario->setCurrentIndex(qMax(0, m_scenario->findData(run["scenario"].toString())));
        m_input->setPlainText(run["input"].toString());
    } else {
        m_graph = {};
        m_title->setText(run["name"].toString());
        m_revision->clear();
        m_description->setText(m_preview->live() || graphId.isEmpty() ? "Inspect the run and its recorded steps."
            : "This run was recorded before its blueprint was deleted.");
        m_publish->hide();
        m_reload->hide();
        m_tabs->setTabEnabled(0, false);
        m_tabs->setTabEnabled(2, false);
        m_runSetup->hide();
        findChild<Button *>("blueprintEdit")->hide();
        findChild<Button *>("blueprintDuplicate")->hide();
        findChild<Button *>("blueprintDelete")->hide();
        m_pages->setCurrentWidget(m_workspace);
    }
    m_tabs->setCurrentIndex(1);
    m_runId = id;
    m_backToChat->setVisible(run["source"].toString() == "Local chat");
    m_comparison = false;
    refreshRun();
    m_traceScroll->verticalScrollBar()->setValue(0);
}

void BlueprintPanel::refreshRun() {
    auto *focus = QApplication::focusWidget();
    const auto focusedStep = focus ? focus->property("blueprintStep").toString() : QString{};
    const auto focusedApproval = focus ? focus->property("approvalFocus").toString() : QString{};
    clear(m_trace);
    const auto run = m_preview->run(m_runId);
    auto *head = new QHBoxLayout;
    head->setContentsMargins(0, 3, 3, 3);
    head->setSpacing(8);
    head->addWidget(label(run["target"].toString().isEmpty() ? "Run details" : "Node test", "blueprintSection"));
    head->addStretch();
    if (!run.isEmpty()) {
        head->addWidget(statusLabel(run["status"].toString()));
        if (running(run)) {
            auto *cancel = action("Cancel run");
            cancel->setEnabled(!m_preview->live() || m_preview->connected());
            connect(cancel, &Button::clicked, this, [this] { m_preview->cancel(m_runId); });
            head->addWidget(cancel);
        }
    }
    m_trace->addLayout(head);
    if (!run.isEmpty()) {
        auto *id = wrapped(m_runId, "blueprintCode");
        id->setAccessibleName("Run ID: " + m_runId);
        id->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        m_trace->addWidget(id);
    }
    m_trace->addWidget(rule());
    if (run.isEmpty()) {
        m_trace->addSpacing(14);
        m_trace->addWidget(wrapped("Run a blueprint or test a node to see its steps here."));
        m_trace->addStretch();
        return;
    }

    const auto steps = run["steps"].toArray();
    const auto state = run["status"].toString();
    const auto decision = run["decision"].toObject();
    const auto decisions = decisionsFor(run);
    const auto runError = run["error"].toString();
    if (!runError.isEmpty()) m_trace->addWidget(wrapped(runError, "blueprintError"));
    const auto actionError = run["actionError"].toString();
    if (!actionError.isEmpty()) m_trace->addWidget(wrapped(actionError, "blueprintError"));
    if (m_traceId != m_runId || m_traceStatus != state) {
        if (m_traceId != m_runId) {
            m_expanded.clear();
            m_baseline = {};
        }
        for (const auto &value : steps) {
            const auto step = value.toObject();
            const auto status = step["status"].toString();
            if (status == "failed" || status == "waiting" || status == "denied" || status == "expired")
                m_expanded.insert(step["id"].toString(), true);
        }
        m_traceId = m_runId;
        m_traceStatus = state;
    }
    int completed = 0;
    QJsonObject priceOutput;
    for (const auto &value : steps) {
        const auto step = value.toObject();
        completed += step["status"].toString() == "passed";
        if (BlueprintPreview::operationId(step) == "pricing") priceOutput = step["output"].toObject();
    }
    const auto elapsed = run.contains("ended") ? BlueprintPreview::duration(run["ended"].toInteger() - run["created"].toInteger())
                                               : QString("In progress");
    const auto source = run["source"].toString();
    const auto revision = run["revision"].toInt() > 0 ? QString(" · Rev %1").arg(run["revision"].toInt()) : QString{};
    auto *summary = new QHBoxLayout;
    summary->setContentsMargins(0, 4, 0, 4);
    summary->addWidget(wrapped(QString("%1  ·  %2 / %3 steps  ·  %4%5").arg(elapsed).arg(completed).arg(steps.size()).arg(source, revision)), 1);
    const auto fixtures = m_preview->fixtures();
    const auto sample = findById(fixtures["samples"].toArray(), run["sampleId"].toString());
    const auto baseline = fixtures["baseline"].toObject();
    const auto baseResult = baseline["outputs"].toObject().value(run["sampleId"].toString()).toObject();
    const bool compare = state == "passed" && !baseResult.isEmpty() && !priceOutput.isEmpty() && decision.isEmpty()
        && run["target"].toString().isEmpty() && sampleText(sample) == run["input"].toString();
    if (compare) {
        auto *button = action(m_comparison ? "Close comparison" : "Compare baseline", false, true);
        summary->addWidget(button);
        connect(button, &Button::clicked, this, [this] { m_comparison = !m_comparison; refreshRun(); });
    }
    if (m_preview->live() && state == "passed" && run["details"].toBool() && !run["toolChat"].toBool()) {
        QJsonArray candidates;
        for (const auto &value : m_preview->runs(run["blueprintId"].toString())) {
            const auto item = value.toObject();
            if (item["id"] != run["id"] && item["status"] == "passed" && item["target"] == run["target"])
                candidates.append(item);
        }
        if (!candidates.isEmpty()) {
            auto *button = action(m_baseline.isEmpty() ? "Compare runs" : "Close comparison", false, true);
            summary->addWidget(button);
            connect(button, &Button::clicked, this, [this, candidates] {
                if (!m_baseline.isEmpty()) { m_baseline = {}; refreshRun(); return; }
                SettingsDetailDialog dialog(this);
                dialog.setWindowTitle("Choose a comparison run");
                auto *body = new QVBoxLayout(&dialog);
                titleBar(body, dialog.windowTitle());
                auto *picker = new ComboBox;
                for (const auto &value : candidates) {
                    const auto item = value.toObject();
                    const auto date = QDateTime::fromMSecsSinceEpoch(item["created"].toInteger()).toString("d MMM, h:mm:ss AP");
                    const auto rev = item["revision"].toInt();
                    picker->addItem(date + (rev ? QString(" · Rev %1").arg(rev) : " · Draft node"), item["id"].toString());
                }
                Surface::field(body, "Completed run", picker);
                auto *buttons = dialogButtons(&dialog, body, "Compare");
                connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
                dialog.resize(430, dialog.sizeHint().height());
                if (dialog.exec() != QDialog::Accepted) return;
                const auto current = m_runId;
                m_preview->loadRun(picker->currentData().toString(), [self = QPointer(this), current](const auto &record, const auto &error) {
                    if (!self || self->m_runId != current) return;
                    if (!error.isEmpty()) { Ui::showNotice(self, "Compare runs", error); return; }
                    self->m_baseline = record;
                    self->refreshRun();
                });
            });
        }
    }
    m_trace->addLayout(summary);
    if (!m_baseline.isEmpty()) {
        m_trace->addWidget(rule());
        m_trace->addWidget(label("Run comparison", "blueprintSection"));
        m_trace->addWidget(wrapped(run["supplied"] == m_baseline["supplied"] ? "Same input values" : "Different input values"));
        auto *comparison = new QHBoxLayout;
        auto *current = new QVBoxLayout;
        auto *baseline = new QVBoxLayout;
        fillCode(current, "Current · " + elapsed, run["result"]);
        fillCode(baseline, "Comparison · " + BlueprintPreview::duration(
            m_baseline["ended"].toInteger() - m_baseline["created"].toInteger()), m_baseline["result"]);
        comparison->addLayout(current, 1);
        comparison->addLayout(baseline, 1);
        m_trace->addLayout(comparison);
    } else if (m_preview->live() && state == "passed" && run["details"].toBool()) {
        auto *result = new QVBoxLayout;
        fillCode(result, "Result", run["result"]);
        m_trace->addLayout(result);
    }
    if (compare && m_comparison) {
        m_trace->addWidget(rule());
        m_trace->addSpacing(7);
        m_trace->addWidget(label("Baseline comparison", "blueprintSection"));
        m_trace->addWidget(wrapped("Same sample input · fixture results"));
        auto *comparison = new QHBoxLayout;
        auto *workflow = new QVBoxLayout;
        auto *baselineBody = new QVBoxLayout;
        const QString observedTotal = QString("$%1 · %2").arg(priceOutput["total"].toDouble(), 0, 'f', 2).arg(elapsed);
        const QString baselineTotal = QString("$%1 · %2").arg(baseResult["total"].toDouble(), 0, 'f', 2)
            .arg(BlueprintPreview::duration(baseline["duration"].toInteger()));
        fillCode(workflow, run["name"].toString() + "\n" + observedTotal, priceOutput);
        fillCode(baselineBody, baseline["name"].toString() + "\n" + baselineTotal, baseResult);
        comparison->addLayout(workflow, 1);
        comparison->addLayout(baselineBody, 1);
        m_trace->addLayout(comparison);
    }

    m_trace->addWidget(rule());
    for (const auto &value : steps) {
        const auto step = value.toObject();
        const auto id = step["id"].toString();
        const auto status = step["status"].toString();
        const auto name = step["name"].toString();
        auto *row = new QHBoxLayout;
        row->setContentsMargins(0, 0, 5, 0);
        row->setSpacing(8);
        auto *button = new QPushButton(literalButtonText(name));
        button->setObjectName("blueprintStep");
        button->setProperty("blueprintStep", id);
        button->setAccessibleName(BlueprintPreview::statusLabel(status) + " · " + name);
        button->setIcon(Theme::coloredIcon(statusIcon(status), statusTone(status), 17));
        button->setCheckable(true);
        button->setChecked(m_expanded.value(id));
        row->addWidget(button, 1);
        row->addWidget(label(BlueprintPreview::duration(step["duration"].toInteger(-1)), "blueprintCode"));
        auto *arrow = label(button->isChecked() ? "⌄" : "›", "cardMeta");
        row->addWidget(arrow);
        m_trace->addLayout(row);
        if (step.contains("error")) {
            auto *error = wrapped(step["error"].toString(), "blueprintStepError");
            m_trace->addWidget(error);
        }
        auto *info = new QWidget;
        info->setObjectName("blueprintStepDetail");
        auto *details = new QVBoxLayout(info);
        details->setContentsMargins(31, 8, 9, 12);
        for (const auto &value : decisions) {
            const auto item = value.toObject();
            if (item["stepId"] == step["id"]) details->addWidget(approvalView(m_preview, run, item));
        }
        auto *io = new QHBoxLayout;
        auto *input = new QVBoxLayout;
        auto *output = new QVBoxLayout;
        fillCode(input, "Input", step["inputs"]);
        const auto outPorts = step["out"].toArray();
        const auto outputTitle = step.contains("operation") && !outPorts.isEmpty()
            ? "Output · " + m_preview->portLabel(outPorts.first().toObject()) : QString("Output");
        fillCode(output, outputTitle, step["output"]);
        io->addLayout(input, 1);
        io->addLayout(output, 1);
        details->addLayout(io);
        m_trace->addWidget(info);
        info->setVisible(button->isChecked());
        m_trace->addWidget(rule());
        connect(button, &QPushButton::toggled, info, [this, info, arrow, id](bool expanded) {
            m_expanded.insert(id, expanded);
            info->setVisible(expanded);
            arrow->setText(expanded ? "⌄" : "›");
        });
        if (id == focusedStep) button->setFocus();
    }
    if (!focusedApproval.isEmpty())
        for (auto *button : m_trace->parentWidget()->findChildren<Button *>())
            if (button->isVisible() && button->property("approvalFocus").toString() == focusedApproval) button->setFocus();
    m_trace->addStretch();
}

void BlueprintPanel::refreshHistory() {
    clear(m_history);
    titleBar(m_history, m_preview->live() ? "Recent run history" : "Run history");
    const auto runs = m_preview->runs(m_graph["id"].toString());
    if (runs.isEmpty()) m_history->addWidget(wrapped("Start a run to see it in this history."));
    for (const auto &value : runs) {
        const auto run = value.toObject();
        const auto id = run["id"].toString();
        auto *row = new QHBoxLayout;
        row->setContentsMargins(0, 5, 0, 5);
        auto *title = new QVBoxLayout;
        title->addWidget(label(run["target"].toString().isEmpty() ? "Full blueprint" : "Node test", "headingLabel"));
        const auto rev = run["revision"].toInt();
        title->addWidget(wrapped(id + (rev ? QString(" · Rev %1").arg(rev) : " · Draft node"), "blueprintCode"));
        row->addLayout(title, 1);
        row->addWidget(statusLabel(run["status"].toString()));
        row->addWidget(label(run["source"].toString(), "cardMeta"));
        row->addWidget(label(run.contains("ended")
            ? BlueprintPreview::duration(run["ended"].toInteger() - run["created"].toInteger()) : "—", "blueprintCode"));
        row->addWidget(label(QDateTime::fromMSecsSinceEpoch(run["created"].toInteger()).toString("d MMM, h:mm AP"), "cardMeta"));
        auto *open = action("Inspect", false, true);
        row->addWidget(open);
        connect(open, &Button::clicked, this, [this, id] { openRun(id); });
        m_history->addLayout(row);
        m_history->addWidget(rule());
    }
    m_history->addStretch();
}

void BlueprintPanel::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    if (m_dark == Theme::isDark()) return;
    m_dark = Theme::isDark();
    QTimer::singleShot(0, this, [this] {
        refreshCanvasActions();
        refreshEditor();
        refreshRun();
        if (m_tabs->currentIndex() == 2) refreshHistory();
    });
}

BlueprintChatPanel::BlueprintChatPanel(BlueprintPreview *preview, QWidget *parent)
    : QWidget(parent), m_preview(preview), m_dark(Theme::isDark()) {
    setObjectName("blueprintChat");
    setAttribute(Qt::WA_StyledBackground, true);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 0, 12, 8);
    auto *header = new QHBoxLayout;
    header->addWidget(label("Local chat", "blueprintTitle"), 1);
    if (!m_preview->live()) header->addWidget(label("Fixture data", "cardMeta"));
    layout->addLayout(header);
    layout->addWidget(rule());
    m_scroll = scroller(&m_messages);
    m_messages->setContentsMargins(8, 8, 12, 8);
    m_messages->setSpacing(9);
    layout->addWidget(m_scroll, 1);
    auto *composer = Surface::card(this);
    composer->parentWidget()->setObjectName("blueprintChatComposer");
    m_input = new QPlainTextEdit;
    m_input->setObjectName("blueprintChatInput");
    m_input->setAccessibleName("Blueprint or tool message");
    m_input->setTabChangesFocus(true);
    m_input->setMaximumHeight(78);
    composer->addWidget(m_input);
    m_inputHint = wrapped({});
    m_inputHint->hide();
    composer->addWidget(m_inputHint);
    m_error = wrapped({}, "blueprintError");
    m_error->hide();
    composer->addWidget(m_error);
    auto *actions = new QHBoxLayout;
    m_mode = new ComboBox;
    m_mode->setAccessibleName("Chat mode");
    m_mode->addItems({"Chat", "Blueprint", "Tool mode"});
    m_blueprint = new ComboBox;
    m_blueprint->setAccessibleName("Saved blueprint");
    m_blueprint->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_blueprint->setMinimumContentsLength(16);
    auto *toolNote = label("Actions require approval", "cardMeta");
    m_send = action("Send ↑", true);
    actions->addWidget(m_mode);
    actions->addWidget(m_blueprint);
    actions->addWidget(toolNote);
    actions->addStretch();
    actions->addWidget(m_send);
    composer->addLayout(actions);
    layout->addWidget(composer->parentWidget());
    connect(m_send, &Button::clicked, this, &BlueprintChatPanel::send);
    auto *sendKey = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), m_input);
    sendKey->setContext(Qt::WidgetShortcut);
    connect(sendKey, &QShortcut::activated, this, &BlueprintChatPanel::send);
    connect(m_mode, &ComboBox::currentIndexChanged, this, [this, toolNote](int index) {
        if (index == 0) {
            emit ordinaryChatRequested();
            return;
        }
        m_error->hide();
        m_blueprint->setVisible(index == 1);
        toolNote->setVisible(index == 2);
        m_input->setPlaceholderText(index == 1 ? "Enter an order message…" : "Message the local model…");
        refreshComposer();
    });
    connect(m_preview, &BlueprintPreview::savedChanged, this, &BlueprintChatPanel::refreshPicker);
    connect(m_preview, &BlueprintPreview::runChanged, this, [this](const QString &id) {
        if (m_preview->run(id)["source"].toString() == "Local chat") rebuildTranscript();
    });
    connect(m_blueprint, &ComboBox::currentIndexChanged, this, &BlueprintChatPanel::refreshComposer);
    connect(m_preview, &BlueprintPreview::serviceChanged, this, [this] {
        refreshComposer();
        rebuildTranscript();
    });
    refreshPicker();
    rebuildTranscript();
}

void BlueprintChatPanel::setMode(int index) {
    m_mode->setCurrentIndex(index);
    m_input->setFocus();
}

void BlueprintChatPanel::refreshPicker() {
    const auto selected = m_blueprint->currentData().toString();
    {
        const QSignalBlocker block(m_blueprint);
        m_blueprint->clear();
        m_blueprint->addItem("Choose a blueprint");
        const auto choices = m_preview->live() ? m_preview->published() : m_preview->saved();
        for (const auto &value : choices) {
            const auto graph = value.toObject();
            if (graph["id"].toString() == "order-intake") continue;
            const auto revision = graph["revision"].toInteger();
            const auto name = graph["name"].toString();
            m_blueprint->addItem(m_preview->live() ? QString("%1 · Rev %2").arg(name).arg(revision) : name,
                graph["id"].toString());
            m_blueprint->setItemData(m_blueprint->count() - 1, revision, Qt::UserRole + 1);
        }
        if (!m_preview->live()) m_blueprint->addItem("Order intake example", "order-intake");
        m_blueprint->setCurrentIndex(qMax(0, m_blueprint->findData(selected)));
    }
    if (m_preview->live() && m_mode->currentIndex() == 1) refreshComposer();
}

void BlueprintChatPanel::refreshComposer() {
    if (!m_preview->live() || m_mode->currentIndex() == 0) return;
    const auto request = ++m_selection;
    m_send->setEnabled(false);
    m_inputHint->show();
    if (!m_preview->connected()) {
        m_inputHint->setText("Waiting for the local service.");
        return;
    }
    if (m_mode->currentIndex() == 1) {
        m_chatGraph = {};
        const auto id = m_blueprint->currentData().toString();
        if (id.isEmpty()) {
            m_input->show();
            m_input->setPlaceholderText("Choose a saved blueprint…");
            m_inputHint->setText("Save a revision in Blueprints to run it here.");
            return;
        }
        m_input->show();
        m_input->setPlaceholderText("Enter workflow inputs…");
        m_inputHint->setText("Reading the saved revision…");
        const auto revision = m_blueprint->currentData(Qt::UserRole + 1).toLongLong();
        m_preview->getBlueprint(id, true, [self = QPointer(this), request](const auto &graph, const auto &error) {
            if (!self || self->m_selection != request || self->m_mode->currentIndex() != 1) return;
            self->m_chatGraph = graph;
            const auto contracts = BlueprintPreview::inputContracts(graph, {});
            self->m_input->setVisible(!contracts.isEmpty() || !error.isEmpty());
            self->m_inputHint->setText(error.isEmpty() ? inputHint(contracts, false) : error);
            self->m_send->setEnabled(error.isEmpty() && !self->m_starting);
        }, revision);
        return;
    }
    m_input->show();
    m_inputHint->setText("Checking the loaded model…");
    m_toolAvailable = false;
    m_preview->toolCapability([self = QPointer(this), request](const auto &capability, const auto &error) {
        if (!self || self->m_selection != request || self->m_mode->currentIndex() != 2) return;
        self->m_toolAvailable = error.isEmpty();
        self->m_inputHint->setText(error.isEmpty()
            ? "Model: " + capability["model"].toString() + " · External actions need approval."
            : error + " Switch to Chat to load a supported local model.");
        self->m_send->setEnabled(self->m_toolAvailable && !self->m_starting);
    });
}

void BlueprintChatPanel::send() {
    const auto text = m_input->toPlainText().trimmed();
    const bool tool = m_mode->currentIndex() == 2;
    if (m_preview->live()) {
        if (!m_send->isEnabled() || m_starting || (tool && text.isEmpty())) return;
        QString error;
        const auto original = m_input->toPlainText();
        const auto graph = m_chatGraph;
        const auto contracts = BlueprintPreview::inputContracts(graph, {});
        const QJsonValue input = tool ? QJsonValue(original) : QJsonValue(collectInput(contracts, original, &error));
        m_error->setText(error);
        m_error->setVisible(!error.isEmpty());
        if (!error.isEmpty()) return;
        m_starting = true;
        m_send->setEnabled(false);
        const auto selection = m_selection;
        m_preview->runLive(graph, input, {}, true, tool, [self = QPointer(this), original, selection, tool]
            (const auto &run, const auto &error) {
            if (!self) return;
            self->m_starting = false;
            if (self->m_selection != selection) { self->refreshComposer(); return; }
            if (tool && !error.isEmpty()) self->refreshComposer();
            else self->m_send->setEnabled(self->m_preview->connected());
            self->m_error->setText(error);
            self->m_error->setVisible(!error.isEmpty());
            if (run.isEmpty()) return;
            if (self->m_input->toPlainText() == original) self->m_input->clear();
            auto *bar = self->m_scroll->verticalScrollBar();
            bar->setValue(bar->maximum());
        });
        return;
    }
    if (text.isEmpty()) return;
    if (!tool && m_blueprint->currentData().toString().isEmpty()) {
        m_error->setText("Choose a saved blueprint.");
        m_error->show();
        m_blueprint->setFocus();
        return;
    }
    QString error;
    const auto graph = tool ? QJsonObject{} : m_preview->blueprint(m_blueprint->currentData().toString());
    const QString scenario = !tool ? "normal"
        : text.contains("version", Qt::CaseInsensitive) || text.contains("command", Qt::CaseInsensitive) ? "command" : "approval";
    m_preview->start(graph, text, {}, scenario, "Local chat", &error);
    m_error->setText(error);
    m_error->setVisible(!error.isEmpty());
    if (error.isEmpty()) {
        m_input->clear();
        auto *bar = m_scroll->verticalScrollBar();
        bar->setValue(bar->maximum());
    }
}

void BlueprintChatPanel::rebuildTranscript() {
    auto *scrollbar = m_scroll->verticalScrollBar();
    const bool follow = scrollbar->value() >= scrollbar->maximum() - 20;
    const auto *focus = QApplication::focusWidget();
    const auto focusedApproval = focus ? focus->property("approvalFocus").toString() : QString{};
    clear(m_messages);
    const auto runs = m_preview->chatRuns();
    if (runs.isEmpty()) {
        m_messages->addStretch();
        m_messages->addWidget(label("What are you working on?", "blueprintTitle"), 0, Qt::AlignHCenter);
        if (m_preview->live()) {
            m_messages->addWidget(wrapped("Choose a saved blueprint, or use Tool mode to ask the loaded local model."), 0, Qt::AlignHCenter);
        } else {
        auto *prompts = new QHBoxLayout;
        prompts->addStretch();
        auto *order = action("Try order intake");
        auto *tool = action("Try a tool request");
        prompts->addWidget(order);
        prompts->addWidget(tool);
        prompts->addStretch();
        m_messages->addLayout(prompts);
        connect(order, &Button::clicked, this, [this] {
            setMode(1);
            m_blueprint->setCurrentIndex(m_blueprint->findData("order-intake"));
            m_input->setPlainText(sampleText(m_preview->fixtures()["samples"].toArray().first().toObject()));
        });
        connect(tool, &Button::clicked, this, [this] {
            setMode(2);
            m_input->setPlainText("Open the sample order preview.");
        });
        }
    }
    for (const auto &value : runs) {
        const auto run = value.toObject();
        if (!m_preview->live() || run["input"].isString()) {
            auto *input = Surface::card(this, "You");
            auto *message = wrapped(run["input"].toString(), "blueprintMessage");
            message->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
            input->addWidget(message);
            m_messages->addWidget(input->parentWidget());
        }
        m_messages->addWidget(label("Magnite", "cardMeta"));
        auto *body = Surface::card(this);
        auto *head = new QHBoxLayout;
        head->addWidget(label(run["name"].toString(), "headingLabel"), 1);
        const auto status = run["status"].toString();
        head->addWidget(statusLabel(status));
        body->addLayout(head);
        const auto steps = run["steps"].toArray();
        if (running(run)) {
            int count = 0;
            for (const auto &step : steps) count += step.toObject()["status"].toString() == "passed";
            auto *progress = new QProgressBar;
            progress->setAccessibleName("Workflow progress");
            progress->setRange(0, qMax(1, int(steps.size())));
            progress->setValue(count);
            body->addWidget(progress);
            const auto cursor = run["cursor"].toInt(-1);
            if (cursor >= 0 && cursor < steps.size())
                body->addWidget(wrapped(steps.at(cursor).toObject()["name"].toString()));
        } else if (status == "passed") {
            if (m_preview->live()) {
                const auto result = run["result"];
                if (!result.isUndefined()) {
                    auto *output = wrapped(result.isString() ? result.toString() : BlueprintPreview::jsonText(result),
                        result.isString() ? "blueprintMessage" : "blueprintCode");
                    output->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
                    body->addWidget(output);
                }
            } else {
            bool displayed = false;
            for (const auto &step : steps) {
                const auto entry = step.toObject();
                const auto operation = BlueprintPreview::operationId(entry);
                if (operation == "output") {
                    const auto port = entry["out"].toArray().first().toObject();
                    body->addWidget(wrapped(m_preview->portLabel(port), "blueprintSection"));
                    body->addWidget(wrapped(BlueprintPreview::jsonText(entry["output"]), "blueprintCode"));
                    displayed = true;
                    continue;
                }
                if (operation != "pricing") continue;
                const auto output = entry["output"].toObject();
                displayed = true;
                body->addWidget(wrapped(QString("%1 × %2 to %3").arg(output["quantity"].toInt())
                    .arg(output["item"].toString(), output["city"].toString()), "blueprintSection"));
                body->addWidget(wrapped(QString("$%1 total · $%2 shipping").arg(output["total"].toDouble(), 0, 'f', 2)
                    .arg(output["shipping"].toDouble(), 0, 'f', 2)));
            }
            if (!displayed && !steps.isEmpty() && run["decision"].isUndefined())
                body->addWidget(wrapped(BlueprintPreview::jsonText(steps.last().toObject()["output"]), "blueprintCode"));
            }
        } else if (status == "failed") {
            const auto cursor = run["cursor"].toInt(-1);
            const auto step = cursor >= 0 && cursor < steps.size() ? steps.at(cursor).toObject() : QJsonObject{};
            const auto error = run["error"].toString(step["error"].toString("The run failed."));
            body->addWidget(wrapped(error, "blueprintError"));
        }
        if (run["actionError"].isString()) body->addWidget(wrapped(run["actionError"].toString(), "blueprintError"));
        for (const auto &value : decisionsFor(run)) {
            body->addWidget(rule());
            body->addWidget(approvalView(m_preview, run, value.toObject()));
        }
        auto *actions = new QHBoxLayout;
        auto *open = action("Open full run", false, true);
        const auto id = run["id"].toString();
        connect(open, &Button::clicked, this, [this, id] { emit runRequested(id); });
        actions->addWidget(open);
        if (running(run)) {
            auto *cancel = action("Cancel run", false, true);
            cancel->setEnabled(!m_preview->live() || m_preview->connected());
            connect(cancel, &Button::clicked, this, [this, id] { m_preview->cancel(id); });
            actions->addWidget(cancel);
        }
        actions->addStretch();
        body->addLayout(actions);
        m_messages->addWidget(body->parentWidget());
    }
    m_messages->addStretch();
    if (!focusedApproval.isEmpty())
        for (auto *button : m_messages->parentWidget()->findChildren<Button *>())
            if (button->isVisible() && button->property("approvalFocus").toString() == focusedApproval) button->setFocus();
    if (follow) QTimer::singleShot(0, m_scroll, [scrollbar] { scrollbar->setValue(scrollbar->maximum()); });
}

void BlueprintChatPanel::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    if (m_dark == Theme::isDark()) return;
    m_dark = Theme::isDark();
    QTimer::singleShot(0, this, &BlueprintChatPanel::rebuildTranscript);
}
