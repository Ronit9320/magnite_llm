#include "panels/blueprintpanel.h"
#include "textformat.h"

#include "panels/blueprintpreview.h"
#include "panels/blueprintbindingfields.h"
#include "panels/blueprintinspector.h"
#include "panels/blueprintlibrarydock.h"
#include "panels/blueprintnodedialog.h"
#include "panels/chatbubble.h"
#include "theme.h"
#include "widgets/blueprintscene.h"
#include "widgets/blueprintstyle.h"
#include "widgets/blueprintview.h"
#include "widgets/brandmark.h"
#include "widgets/button.h"
#include "widgets/combobox.h"
#include "widgets/confirmation.h"
#include "widgets/edgescrollbar.h"
#include "widgets/floatingpanel.h"
#include "widgets/settingswindow.h"
#include "widgets/spinbox.h"
#include "widgets/surface.h"
#include "widgets/tabwidget.h"

#include <QApplication>
#include <QDateTime>
#include <QDialogButtonBox>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QPainter>
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
#include <QScreen>
#include <QSettings>
#include <QSignalBlocker>
#include <QShortcut>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
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

// The graph without the first edge matching all four fields of edge; found reports whether one matched.
QJsonObject withoutEdge(QJsonObject graph, const QJsonObject &edge, bool *found = nullptr) {
    QJsonArray edges;
    bool matched = false;
    for (const auto &value : graph["edges"].toArray()) {
        const auto existing = value.toObject();
        if (!matched && existing["from"] == edge["from"] && existing["out"] == edge["out"]
                && existing["to"] == edge["to"] && existing["in"] == edge["in"]) {
            matched = true;
            continue;
        }
        edges.append(existing);
    }
    graph["edges"] = edges;
    if (found) *found = matched;
    return graph;
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
    auto *pane = new QFrame;
    pane->setObjectName("runToolCall");
    auto *body = new QVBoxLayout(pane);
    body->setContentsMargins(12, 8, 12, 8);
    body->setSpacing(6);
    const auto status = decision["status"].toString();
    const auto outcome = decision["outcome"].toString();
    auto *header = new QHBoxLayout;
    auto *icon = label({});
    const auto kind = decision["type"].toString();
    icon->setPixmap(Theme::coloredIcon(kind == "Read file or folder" ? "file-text"
        : decision.contains("target") || kind == "Open target" ? "folder" : "play",
        statusTone(status), 16).pixmap(16, 16));
    header->addWidget(icon);
    header->addWidget(label(decision["type"].toString(), "headingLabel"));
    header->addStretch();
    header->addWidget(statusLabel(status));
    body->addLayout(header);
    auto *fields = new QWidget(pane);
    auto *fieldLayout = new QVBoxLayout(fields);
    fieldLayout->setContentsMargins(0, 0, 0, 0);
    fieldLayout->setSpacing(4);
    const auto detail = [fieldLayout](const QString &name, const QJsonValue &value) {
        auto *line = wrapped(name + "  " + BlueprintPreview::jsonText(value), "blueprintCode");
        line->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        fieldLayout->addWidget(line);
    };
    if (decision.contains("executable")) {
        detail("Executable", decision["executable"]);
        detail("Arguments", decision["args"]);
        detail("Working directory", decision["cwd"]);
    } else if (decision.contains("target")) {
        detail("Target", decision["target"]);
    }
    if (fieldLayout->count() && status != "waiting") {
        auto *toggle = action("Show action details", false, true);
        toggle->setCheckable(true);
        body->addWidget(toggle, 0, Qt::AlignLeft);
        fields->hide();
        QObject::connect(toggle, &Button::toggled, fields, [fields, toggle](bool open) {
            toggle->setText(open ? "Hide action details" : "Show action details");
            fields->setVisible(open);
        });
    }
    if (fieldLayout->count()) body->addWidget(fields);
    else delete fields;
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
    } else if (preview->live()) {
        static const QHash<QString, QString> outcomes{
            {"dispatched", "Passed to the desktop handler."},
            {"handler_unavailable", "No desktop handler was available."},
            {"dispatch_failed", "The action could not be started."},
            {"nonzero_exit", "The process exited with an error."},
            {"timed_out", "The command timed out."},
            {"backend_interrupted", "The local service was interrupted."}
        };
        if (outcomes.contains(outcome)) body->addWidget(wrapped(outcomes.value(outcome)));
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
    // The library page lines up with the 32px gutter the other panels use;
    // the editor keeps the tighter frame for the canvas.
    library->setContentsMargins(12, 0, 12, 0);   // 20px root + 12 = the 32px BasePanel gutter
    m_service = wrapped({}, "blueprintWarning");
    m_service->hide();
    library->addWidget(m_service);
    library->addWidget(scroller(&m_libraryBody), 1);

    // The editor is full-bleed; the library and create pages keep the panel gutter.
    connect(m_pages, &QStackedWidget::currentChanged, this, [this, root] {
        if (m_pages->currentWidget() == m_workspace) root->setContentsMargins(0, 0, 0, 0);
        else root->setContentsMargins(20, 17, 20, 14);
    });
    auto *workspace = new QVBoxLayout(m_workspace);
    workspace->setContentsMargins(0, 0, 0, 0);
    workspace->setSpacing(0);
    // Outside Build the bar sits above the page; the slot puts its body on the pages' 20px gutter.
    const int shadow = FloatingPanel::shadowMargin();
    m_barSlot = new QVBoxLayout;
    m_barSlot->setContentsMargins(20 - shadow, 0, 20 - shadow, 0);
    workspace->addLayout(m_barSlot);
    m_bar = new BlueprintTopBar;
    m_barSlot->addWidget(m_bar);
    m_views = new QStackedWidget;
    workspace->addWidget(m_views, 1);
    const auto page = [](QWidget *content) {
        auto *inset = new QWidget;
        auto *layout = new QVBoxLayout(inset);
        layout->setContentsMargins(20, 0, 20, 14);
        layout->addWidget(content);
        return inset;
    };
    setupEditor();
    m_views->addWidget(page(setupRun()));
    m_views->addWidget(page(scroller(&m_history)));

    const auto outOfFullscreen = [this] { if (m_fullscreenDialog) m_fullscreenDialog->reject(); };
    connect(m_bar, &BlueprintTopBar::backClicked, this, [this, outOfFullscreen] { outOfFullscreen(); showLibrary(); });
    connect(m_bar, &BlueprintTopBar::viewChanged, this, &BlueprintPanel::showView);
    connect(m_bar, &BlueprintTopBar::issueClicked, this, &BlueprintPanel::checkValidation);
    connect(m_bar, &BlueprintTopBar::publishClicked, this, &BlueprintPanel::publish);
    connect(m_bar, &BlueprintTopBar::reloadClicked, this, [this, outOfFullscreen] { outOfFullscreen(); reloadRevision(); });
    connect(m_bar, &BlueprintTopBar::openInConversationClicked, this, [this, outOfFullscreen] {
        outOfFullscreen();
        emit conversationRequested(m_graph["id"].toString());
    });
    connect(m_bar, &BlueprintTopBar::backToConversationClicked, this, [this, outOfFullscreen] {
        outOfFullscreen();
        emit chatRequested();
    });
    connect(m_bar, &BlueprintTopBar::runClicked, this, [this] {
        // In Test cases, Run starts the case shown (as Start run does); elsewhere it opens Test cases.
        if (m_bar->view() != BlueprintTopBar::View::TestCases) showTest();
        else if (m_start->isEnabled()) startRun();
    });
    connect(m_bar, &BlueprintTopBar::runMenuRequested, this, &BlueprintPanel::showRunMenu);
    connect(m_bar, &BlueprintTopBar::editDetailsClicked, this, [this, outOfFullscreen] { outOfFullscreen(); editDetails(); });
    connect(m_bar, &BlueprintTopBar::duplicateClicked, this, [this, outOfFullscreen] {
        outOfFullscreen();
        if (!m_graph.isEmpty()) duplicate(m_graph);
    });
    connect(m_bar, &BlueprintTopBar::deleteClicked, this, [this, outOfFullscreen] {
        outOfFullscreen();
        deleteBlueprint(m_graph["id"].toString());
    });
    connect(m_bar, &BlueprintTopBar::fullscreenClicked, this, &BlueprintPanel::toggleFullscreen);
    connect(m_preview, &BlueprintPreview::savedChanged, this, &BlueprintPanel::refreshLibrary);
    connect(m_preview, &BlueprintPreview::runChanged, this, [this](const QString &id) {
        if (id == m_runId) refreshRun();
        if (m_bar->view() == BlueprintTopBar::View::RunHistory) refreshHistory();
    });
    connect(m_preview, &BlueprintPreview::serviceChanged, this, [this] {
        const auto error = m_preview->serviceError();
        m_service->setText(error);
        m_service->setVisible(!error.isEmpty());
        if (m_preview->live()) {
            m_bar->setConnected(m_preview->connected());
            if (!m_graph.isEmpty()) refreshRunOptions();
            refreshRun();
        }
    });
    connect(m_preview, &BlueprintPreview::optionsChanged, this, [this] {
        if (!m_graph.isEmpty()) {
            m_canvas->setGraph(m_graph);
            refreshRunOptions();
        }
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

void BlueprintPanel::showRecentRuns() {
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
}

void BlueprintPanel::refreshLibrary() {
    clear(m_libraryBody);
    const auto saved = m_preview->saved();
    auto *header = new QHBoxLayout;
    auto *titles = new QVBoxLayout;
    titles->setSpacing(4);
    titles->addWidget(label("Blueprints", "blueprintTitle"));
    titles->addWidget(wrapped("Chain local models, rules and tools into workflows you can test, rerun and run from Conversation.",
                              "cardMeta"));
    header->addLayout(titles, 1);
    auto *create = action("Create new", true);
    create->setObjectName("blueprintCreate");
    create->setIcon(Theme::coloredIcon("plus", QColor(Theme::accentText()), 16));
    connect(create, &Button::clicked, this, &BlueprintPanel::showCreatePage);
    if (m_preview->live()) {
        auto *recent = action("Recent runs");
        connect(recent, &Button::clicked, this, &BlueprintPanel::showRecentRuns);
        header->addWidget(recent, 0, Qt::AlignTop);
        header->addSpacing(8);
    }
    header->addWidget(create, 0, Qt::AlignTop);
    m_libraryBody->addLayout(header);
    m_libraryBody->addSpacing(18);

    // Ways to start: prominent in an empty library, below the list otherwise.
    auto *starts = new QHBoxLayout;
    starts->setSpacing(10);
    const auto startCard = [this, starts](const QString &icon, const QString &title, const QString &detail,
                                          auto slot) {
        auto *card = new Button;
        card->setObjectName("blueprintStartCard");
        card->setAccessibleName(title);
        card->setAccessibleDescription(detail);
        card->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        card->setFixedHeight(82);
        auto *body = new QVBoxLayout(card);
        body->setContentsMargins(14, 11, 14, 11);
        body->setSpacing(4);
        auto *top = new QHBoxLayout;
        top->setSpacing(7);
        auto *glyph = label({});
        glyph->setFixedSize(16, 16);
        glyph->setPixmap(Theme::coloredIcon(icon, QColor(Theme::accentLabel()), 16).pixmap(16, 16));
        top->addWidget(glyph);
        top->addWidget(label(title, "headingLabel"), 1);
        body->addLayout(top);
        auto *caption = wrapped(detail, "cardMeta");
        caption->setAttribute(Qt::WA_TransparentForMouseEvents);
        body->addWidget(caption);
        for (auto *child : card->findChildren<QLabel *>()) child->setAttribute(Qt::WA_TransparentForMouseEvents);
        connect(card, &Button::clicked, this, slot);
        starts->addWidget(card, 1);
    };
    startCard("plus", "Blank workflow", "Start with an empty canvas and add the steps you need.",
              &BlueprintPanel::showCreatePage);
    if (m_preview->live())
        startCard("file-text", "Request triage example",
                  "Summarize an incoming request with a local model and return a review record.",
                  &BlueprintPanel::createTriageExample);

    if (saved.isEmpty()) {
        m_libraryBody->addWidget(label("Start a workflow", "headingLabel"));
        m_libraryBody->addSpacing(6);
        m_libraryBody->addLayout(starts);
        m_libraryBody->addStretch();
        return;
    }
    m_libraryBody->addWidget(label(m_preview->live() ? "Drafts and saved blueprints" : "Saved blueprints", "headingLabel"));
    for (const auto &value : saved) {
        const auto graph = value.toObject();
        const auto id = graph["id"].toString();
        auto *card = Surface::card(m_library);
        auto *row = new QHBoxLayout;
        auto *text = new QVBoxLayout;
        text->setSpacing(3);
        auto *name = new QHBoxLayout;
        name->addWidget(label(graph["name"].toString(), "headingLabel"));
        const auto revision = graph["revision"].toInt();
        auto *badge = label(revision ? QString("Revision %1").arg(revision) : QString("Draft"), "blueprintRevision");
        name->addWidget(badge, 0, Qt::AlignVCenter);
        if (revision && graph["dirty"].toBool()) {
            auto *unsaved = label("Unsaved edits", "blueprintRevision");
            unsaved->setProperty("state", "warn");
            name->addWidget(unsaved, 0, Qt::AlignVCenter);
        }
        name->addStretch();
        text->addLayout(name);
        const auto description = graph["description"].toString().trimmed();
        if (!description.isEmpty()) text->addWidget(wrapped(description));
        QStringList detail;
        if (graph["nodes"].isArray()) detail << QString("%1 nodes").arg(graph["nodes"].toArray().size());
        const auto recent = m_preview->runs(id);
        if (!recent.isEmpty())
            detail << "Last run " + BlueprintPreview::statusLabel(recent.first().toObject()["status"].toString()).toLower();
        if (!detail.isEmpty()) text->addWidget(wrapped(detail.join(" · "), "cardMeta"));
        row->addLayout(text, 1);
        auto *duplicateButton = iconAction("copy", "Duplicate blueprint");
        auto *remove = iconAction("trash", "Delete blueprint");
        auto *open = action("Open");
        row->addWidget(duplicateButton, 0, Qt::AlignVCenter);
        row->addWidget(remove, 0, Qt::AlignVCenter);
        row->addWidget(open, 0, Qt::AlignVCenter);
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
    m_libraryBody->addSpacing(22);
    m_libraryBody->addWidget(label("Start something new", "headingLabel"));
    m_libraryBody->addSpacing(6);
    m_libraryBody->addLayout(starts);
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
    m_newName->setPlaceholderText("e.g. Request triage");
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

void BlueprintPanel::createTriageExample() {
    auto request = m_preview->makeNode("input", "request", 0, 0);
    request["name"] = "Incoming request";
    auto agent = m_preview->makeNode("generate", "triage", 245, 0);
    agent["name"] = "Summarize request";
    agent["response"] = "text";
    agent["maxTokens"] = 160;
    agent["prompt"] = "Summarize the request in one sentence. Include what the requester needs. Use only the request as evidence.";
    auto status = m_preview->makeNode("constant", "status", 245, 175);
    status["name"] = "Review status";
    status["setting"] = "\"Needs review\"";
    auto record = m_preview->makeNode("build-object", "record", 490, 0);
    record["name"] = "Build triage record";
    const QJsonObject text{{"type", "string"}};
    record["in"] = QJsonArray{
        QJsonObject{{"id", "summary"}, {"name", "Summary"}, {"type", "text"}, {"schema", text}},
        QJsonObject{{"id", "status"}, {"name", "Status"}, {"type", "text"}, {"schema", text}}};
    auto output = m_preview->makeNode("output", "result", 735, 0);
    output["name"] = "Triage record";
    for (const auto &side : {"in", "out"}) {
        output[side] = QJsonArray{QJsonObject{{"id", "value"}, {"name", "Record"}, {"type", "object"},
                                             {"schema", QJsonObject{{"type", "object"}}}}};
    }
    const auto id = BlueprintPreview::id();
    const QJsonObject graph{{"id", id}, {"revision", 0}, {"dirty", true}, {"name", "Request triage"},
        {"description", "Summarize an incoming request with a local model and return a review record."},
        {"nodes", QJsonArray{request, agent, status, record, output}},
        {"edges", QJsonArray{
            QJsonObject{{"from", "request"}, {"out", "value"}, {"to", "triage"}, {"in", "value"}},
            QJsonObject{{"from", "triage"}, {"out", "value"}, {"to", "record"}, {"in", "summary"}},
            QJsonObject{{"from", "status"}, {"out", "value"}, {"to", "record"}, {"in", "status"}},
            QJsonObject{{"from", "record"}, {"out", "value"}, {"to", "result"}, {"in", "value"}}}}};
    if (saveGraph(graph, false)) openBlueprint(id);
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
    if (graph.isEmpty()) return;
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
        m_preview->getBlueprint(id, false, [self = QPointer(this), nav](const QJsonObject &graph, const QString &error) {
            if (!self || nav != self->m_navigation) return;
            if (!error.isEmpty()) { Ui::showNotice(self, "Blueprints", error); return; }
            self->showBlueprint(graph);
        });
        return;
    }
    showBlueprint(m_preview->blueprint(id));
}

void BlueprintPanel::showBlueprint(const QJsonObject &graph) {
    const auto id = graph["id"].toString();
    m_canGoBack = false;
    m_graph = graph;
    if (m_graph.isEmpty()) return;
    m_selected.clear();  // nothing is selected on open, so the inspector stays out of the way
    const auto recent = m_preview->runs(id);
    m_runId = recent.isEmpty() ? QString{} : recent.first().toObject()["id"].toString();
    m_comparison = false;
    m_runSetup->show();
    m_pages->setCurrentWidget(m_workspace);
    refreshEditor(true);
    showTest();
    showView(BlueprintTopBar::View::Build);
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
    m_canvasPages = new QStackedWidget;
    m_views->addWidget(m_canvasPages);
    m_canvas = new BlueprintView(*m_preview);
    m_canvas->installEventFilter(this);
    m_canvas->setDropValidator([this](const QJsonObject &e) {
        return m_preview->dropError(withoutEdge(m_graph, m_canvas->movingEdge()), e);
    });
    m_canvasPages->addWidget(m_canvas);
    auto *empty = new QWidget;
    m_emptyCanvas = empty;
    empty->setObjectName("blueprintEmptyCanvas");
    auto *center = new QVBoxLayout(empty);
    center->addStretch();
    auto *startIcon = label({});
    startIcon->setProperty("themeIcon", "message-circle");
    startIcon->setPixmap(Theme::coloredIcon("message-circle", QColor(Theme::textSecondary()), 20).pixmap(20, 20));
    center->addWidget(startIcon, 0, Qt::AlignHCenter);
    center->addWidget(label("Start with a node", "headingLabel"), 0, Qt::AlignHCenter);
    auto *emptyHint = wrapped(m_preview->live() ? "Start with a run input, dataset, or constant."
                                               : "Use an Input or Dataset node to supply values.");
    emptyHint->setAlignment(Qt::AlignCenter);
    center->addWidget(emptyHint);
    m_emptyAdd = action("Add node");
    connect(m_emptyAdd, &Button::clicked, this, [this] { openNodePicker(); });
    center->addWidget(m_emptyAdd, 0, Qt::AlignHCenter);
    center->addStretch();
    m_canvasPages->addWidget(empty);

    // The node library: operations to add, and the nodes already in this workflow.
    const int shadow = FloatingPanel::shadowMargin();
    m_libraryPanel = new FloatingPanel;
    m_libraryPanel->setObjectName("blueprintLibraryDock");
    m_libraryPanel->setAccessibleName("Node library");
    m_libraryPanel->setFixedWidth(248 + 2 * shadow);
    auto *libraryLayout = new QVBoxLayout(m_libraryPanel);
    libraryLayout->setContentsMargins(4, 4, 0, 0);
    m_libraryDock = new BlueprintLibraryDock(*m_preview);
    libraryLayout->addWidget(m_libraryDock, 1);
    m_canvas->addOverlay(m_libraryPanel, OverlayEdge::Left);
    connect(m_canvas, &BlueprintView::libraryToggled, m_libraryPanel, &QWidget::setVisible);
    m_canvas->setLibraryShown(true);
    connect(m_libraryDock, &BlueprintLibraryDock::addRequested, this,
            [this](const BlueprintLibraryDock::Entry &entry) { addEntry(entry); });
    connect(m_canvas, &BlueprintView::entryDropped, this, [this](const QByteArray &json, const QPointF &graph) {
        addEntry(BlueprintLibraryDock::Entry::fromJson(json), graph);
    });
    connect(m_libraryDock, &BlueprintLibraryDock::nodeActivated, this, [this](const QString &id) {
        m_selected = id;
        m_canvas->revealNode(id);
        refreshInspector();
    });
    // "/" jumps to the library search; text fields keep it as a character.
    auto *search = new QShortcut(QKeySequence(Qt::Key_Slash), m_canvas, nullptr, nullptr, Qt::WidgetWithChildrenShortcut);
    connect(search, &QShortcut::activated, this, [this] {
        m_canvas->setLibraryShown(true);
        m_libraryDock->focusSearch();
    });

    // Today's inspector content, floating at the right while a node is selected.
    m_inspectorPanel = new FloatingPanel;
    m_inspectorPanel->setObjectName("blueprintInspector");
    m_inspectorPanel->setAccessibleName("Node inspector");
    m_inspectorPanel->setFixedWidth(300 + 2 * shadow);
    auto *inspectorLayout = new QVBoxLayout(m_inspectorPanel);
    inspectorLayout->setContentsMargins(4, 4, 0, 0);
    auto *inspectorScroll = scroller(&m_inspector);
    m_inspector->setSpacing(6);
    m_nodeInspector = new BlueprintInspector(*m_preview);
    m_inspector->addWidget(m_nodeInspector);
    inspectorLayout->addWidget(inspectorScroll);
    // Inspector edits apply to the node they were made for, and only while it still exists.
    connect(m_nodeInspector, &BlueprintInspector::nodeChanged, this, [this](const QJsonObject &node, bool ports) {
        if (findById(m_graph["nodes"].toArray(), node["id"].toString()).isEmpty()) return;
        const auto before = m_graph;
        changeNode(node, ports);
        if (m_graph == before) m_nodeInspector->reload();
    });
    connect(m_nodeInspector, &BlueprintInspector::disconnectRequested, this, &BlueprintPanel::removeConnection);
    connect(m_nodeInspector, &BlueprintInspector::schemaRequested, this, &BlueprintPanel::editSchema);
    connect(m_nodeInspector, &BlueprintInspector::trainRequested, this, [this] {
        if (m_preview->live()) liveTraining();
        else saveTrainingRecipe();
    });
    connect(m_nodeInspector, &BlueprintInspector::testRequested, this, [this](const QString &id) { showTest(id); });
    connect(m_nodeInspector, &BlueprintInspector::saveToLibraryRequested, this, &BlueprintPanel::saveNodeTemplate);
    connect(m_nodeInspector, &BlueprintInspector::deleteRequested, this, &BlueprintPanel::removeNode);
    connect(m_nodeInspector, &BlueprintInspector::closeRequested, this, [this] {
        m_selected.clear();
        refreshInspector();
    });
    m_canvas->addOverlay(m_inspectorPanel, OverlayEdge::Right);
    m_inspectorPanel->hide();

    // The dock's Add node opens the node picker until quick-add replaces it.
    connect(m_canvas, &BlueprintView::addNodeClicked, this, [this] { openNodePicker(); });
    connect(m_canvas, &BlueprintView::nodeSelected, this, [this](const QString &id) {
        m_selected = id;
        refreshInspector();
    });
    connect(m_canvas, &BlueprintView::nodeMenuRequested, this, &BlueprintPanel::showNodeMenu);
    connect(m_canvas, &BlueprintView::deleteRequested, this, &BlueprintPanel::removeNode);
    connect(m_canvas, &BlueprintView::nodeMoved, this, [this](const QString &id, const QPoint &position) {
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
    connect(m_canvas, &BlueprintView::connectRequested, this, &BlueprintPanel::dropConnection);
    connect(m_canvas, &BlueprintView::disconnectRequested, this, &BlueprintPanel::removeConnection);
    connect(m_canvas, &BlueprintView::wireMoved, this, &BlueprintPanel::moveConnection);
    // The quick-add popup replaces the node picker for these in a later task.
    connect(m_canvas, &BlueprintView::wireDropped, this,
            [this](const QString &, const QString &, const QPoint &, const QPointF &graph) { openNodePicker(graph); });
    connect(m_canvas, &BlueprintView::quickAddRequested, this,
            [this](const QPoint &, const QPointF &graph) { openNodePicker(graph); });
    connect(m_canvas, &BlueprintView::canvasMenuRequested, this,
            [this](const QPoint &, const QPointF &graph) { openNodePicker(graph); });
}

void BlueprintPanel::checkValidation(int issue) {
    const auto issues = m_preview->issues(m_graph);
    if (issue >= 0 && issue < issues.size()) {
        // Issues start with the node's name; the longest matching name is the offending node.
        QString found;
        qsizetype matched = 0;
        for (const auto &value : m_graph["nodes"].toArray()) {
            const auto node = value.toObject();
            const auto name = node["name"].toString();
            if (!name.isEmpty() && name.size() > matched && issues.at(issue).startsWith(name)) {
                found = node["id"].toString();
                matched = name.size();
            }
        }
        if (found.isEmpty()) {
            Ui::showNotice(this, "Workflow validation", issues.join('\n'));
            return;
        }
        showView(BlueprintTopBar::View::Build);
        m_selected = found;
        m_canvas->revealNode(found);
        refreshInspector();
        return;
    }
    // No issues: the live service checks bindings and the device; fixtures are ready as they are.
    if (!m_preview->live()) {
        Ui::showNotice(this, "Workflow validation", "The workflow has compatible bindings and connected inputs.");
        return;
    }
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
}

void BlueprintPanel::showRunMenu(const QPoint &globalPosition) {
    QMenu menu(m_bar);
    menu.addAction("Open Test cases…", this, [this] {
        if (m_fullscreenDialog) m_fullscreenDialog->reject();
        showView(BlueprintTopBar::View::TestCases);
    });
    menu.exec(globalPosition - QPoint(menu.sizeHint().width(), 0));
}

void BlueprintPanel::refreshEditor(bool fit) {
    if (m_graph.isEmpty()) return;
    refreshBar();
    const auto nodes = m_graph["nodes"].toArray();
    if (!m_fullscreenDialog) m_canvasPages->setCurrentWidget(nodes.isEmpty() ? m_emptyCanvas : m_canvas);
    m_bar->setFullscreenAvailable(m_fullscreenDialog || !nodes.isEmpty());
    placeBar();
    m_canvas->setGraph(m_graph);
    if (fit) m_canvas->fit();
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
    QHash<QString, QString> warnings;
    const auto issues = m_preview->issues(m_graph);
    for (const auto &value : nodes) {
        const auto node = value.toObject();
        warnings.insert(node["id"].toString(),
                        BlueprintScene::warningLines(*m_preview, m_graph, node, issues).join('\n'));
    }
    m_libraryDock->setGraph(m_graph, warnings);
    refreshInspector();
    if (m_preview->live()) refreshRunOptions();
}

void BlueprintPanel::refreshBar() {
    if (m_graph.isEmpty()) {  // a run whose blueprint is not open
        m_bar->setActions(false, false, false, m_canGoBack);
        return;
    }
    const auto revision = m_graph["revision"].toInt();
    const bool dirty = m_graph["dirty"].toBool();
    const bool live = m_preview->live();
    m_bar->setBlueprint(m_graph["name"].toString(),
                        revision ? QString("Revision %1%2").arg(revision).arg(dirty ? " · unpublished edits" : "")
                                 : QString("No published revision"),
                        dirty || !revision, !live);
    // One way back to Conversation is enough.
    m_bar->setActions(live && dirty, live && dirty && revision, live && revision > 0 && !m_canGoBack, m_canGoBack);
    m_bar->setConnected(!live || m_preview->connected());
    m_bar->setValidation(m_preview->issues(m_graph));
    m_bar->setEditable(true);
    m_bar->setRunEnabled(true);
}

void BlueprintPanel::showView(BlueprintTopBar::View view) {
    if (view != BlueprintTopBar::View::Build && m_fullscreenDialog) m_fullscreenDialog->reject();
    m_bar->setView(view);
    m_views->setCurrentIndex(int(view));
    placeBar();
    if (view == BlueprintTopBar::View::RunHistory) {
        refreshHistory();
        m_preview->refreshHistory(m_graph["id"].toString());
    }
}

void BlueprintPanel::placeBar() {
    const bool overCanvas = m_fullscreenDialog
        || (m_views->currentIndex() == int(BlueprintTopBar::View::Build) && m_canvasPages->currentWidget() == m_canvas);
    if (overCanvas == (m_bar->parentWidget() == m_canvas)) return;
    if (overCanvas) {
        m_barSlot->removeWidget(m_bar);
        m_canvas->addOverlay(m_bar, OverlayEdge::Top);
    } else {
        m_canvas->removeOverlay(m_bar);
        m_barSlot->addWidget(m_bar);
        m_bar->show();
    }
}

void BlueprintPanel::applyCompactRule() {
    if (m_selected.isEmpty() || m_inspectorPanel->isHidden() || !m_canvas->libraryShown()) return;
    if (m_canvas->freeArea().width() < 240) m_canvas->setLibraryShown(false);
}

void BlueprintPanel::refreshInspector() {
    m_canvas->setSelection(m_selected);
    const auto nodes = m_graph["nodes"].toArray();
    m_libraryDock->setCurrentNode(m_selected);
    const auto node = findById(nodes, m_selected);
    // The inspector floats in while a node is selected and leaves when nothing is.
    m_inspectorPanel->setVisible(!node.isEmpty());
    m_nodeInspector->setNode(m_graph, node.isEmpty() ? QString{} : m_selected);
    if (!node.isEmpty()) applyCompactRule();
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
            if (removed && !Ui::confirmAction(m_canvas, "Update node",
                    QString("This change will remove %1.").arg(countOf(removed, "incompatible connection")), "Save changes")) return;
            graph["edges"] = retained;
        }
        if (!saveGraph(graph)) refreshEditor();
        return;
    }
}

void BlueprintPanel::openNodePicker(std::optional<QPointF> position) {
    auto *dialog = new SettingsDetailDialog(m_canvas);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle("Add node");
    dialog->setAccessibleName("Add node picker");
    const auto screen = m_canvas->screen()->availableGeometry();
    const int width = qMin(650, screen.width() - 16);
    const bool compact = width < 500 || screen.height() < 420;
    const int horizontalMargin = compact ? 12 : 16;
    const int gridGap = compact ? 6 : 8;
    auto *body = new QVBoxLayout(dialog);
    body->setContentsMargins(horizontalMargin, compact ? 10 : 14, horizontalMargin, compact ? 10 : 14);
    body->setSpacing(compact ? 8 : 10);
    titleBar(body, dialog->windowTitle());
    auto *footer = new QHBoxLayout;
    constexpr int columns = 3;
    constexpr int perPage = 9;
    auto gridPage = [gridGap](QWidget *page) {
        auto *grid = new QGridLayout(page);
        grid->setContentsMargins(0, 4, 0, 0);
        grid->setSpacing(gridGap);
        for (int column = 0; column < columns; ++column)
            grid->setColumnStretch(column, 1);
        return grid;
    };
    const int cardWidth = (width - 2 * horizontalMargin - (columns - 1) * gridGap - 4) / columns;
    auto addCard = [compact, cardWidth](QGridLayout *grid, int index, const QString &title, const QString &detail,
                                       const QString &kind, const QString &actionName) {
        auto *button = new Button;
        button->setObjectName("blueprintNodeCard");
        button->setAutoDefault(false);
        button->setAccessibleName(actionName);
        button->setAccessibleDescription(detail);
        button->setToolTip("<qt>" + title.toHtmlEscaped() + "<br>" + detail.toHtmlEscaped() + "</qt>");
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setFixedHeight(compact ? 78 : 82);
        const int inset = compact ? 8 : 10;
        const int iconSize = compact ? 14 : 16;
        const int iconGap = compact ? 4 : 6;
        auto *contents = new QVBoxLayout(button);
        contents->setContentsMargins(inset, 9, inset, 9);
        contents->setSpacing(5);
        auto *header = new QHBoxLayout;
        header->setSpacing(iconGap);
        auto *icon = label({});
        icon->setFixedSize(iconSize, iconSize);
        icon->setPixmap(Theme::coloredIcon(BlueprintStyle::nodeIcon(kind), BlueprintStyle::familyColor(BlueprintStyle::nodeFamily(kind)),
                                           iconSize).pixmap(iconSize, iconSize));
        auto *name = label(title, "headingLabel");
        name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        name->ensurePolished();
        const int titleWidth = cardWidth - 2 * inset - iconSize - iconGap;
        name->setText(name->fontMetrics().elidedText(title, Qt::ElideRight, titleWidth));
        auto *caption = wrapped(detail);
        for (auto *child : {icon, name, caption}) child->setAttribute(Qt::WA_TransparentForMouseEvents);
        header->addWidget(icon, 0, Qt::AlignTop);
        header->addWidget(name, 1);
        contents->addLayout(header);
        contents->addWidget(caption);
        contents->addStretch();
        grid->addWidget(button, index / columns, index % columns);
        return button;
    };

    const auto saved = m_preview->nodeLibrary();
    auto *builtInPage = new QWidget;
    auto *builtInGrid = gridPage(builtInPage);
    const auto types = m_preview->fixtures()["nodeTypes"].toArray();
    for (int i = 0; i < types.size(); ++i) {
        const auto type = types.at(i).toObject();
        const auto kind = type["id"].toString();
        const auto title = type["label"].toString();
        auto *button = addCard(builtInGrid, i, title, type["detail"].toString(), kind, "Add " + title);
        connect(button, &Button::clicked, dialog, [dialog, kind] {
            dialog->setProperty("nodeKind", kind);
            dialog->accept();
        });
    }
    if (saved.isEmpty()) {
        body->addWidget(builtInPage, 1);
    } else {
        auto *tabs = new TabWidget;
        tabs->addTab(builtInPage, "Built-in nodes");
        auto *savedPage = new QWidget;
        auto *savedLayout = new QVBoxLayout(savedPage);
        savedLayout->setContentsMargins(0, 0, 0, 0);
        auto *pages = new QStackedWidget;
        savedLayout->addWidget(pages, 1);
        tabs->addTab(savedPage, "Saved nodes");
        body->addWidget(tabs, 1);

        for (int start = 0; start < saved.size(); start += perPage) {
            auto *page = new QWidget;
            auto *grid = gridPage(page);
            const auto end = qMin(qsizetype(start + perPage), saved.size());
            for (int i = start; i < end; ++i) {
                const auto entry = saved.at(i).toObject();
                const auto design = entry["node"].toObject();
                const auto spec = m_preview->operation(BlueprintPreview::operationId(design));
                const auto title = entry["name"].toString();
                auto *button = addCard(grid, i - start, title, spec["label"].toString(),
                                       spec["kind"].toString(), "Add saved node " + title);
                connect(button, &Button::clicked, dialog, [dialog, design] {
                    dialog->setProperty("savedNode", QVariant::fromValue(design));
                    dialog->accept();
                });
                button->setContextMenuPolicy(Qt::CustomContextMenu);
                connect(button, &QWidget::customContextMenuRequested, dialog, [this, button, dialog, entry](const QPoint &point) {
                    QMenu menu(dialog);
                    menu.addAction("Remove from node library…", this, [this, dialog, entry] {
                        if (!Ui::confirmRemoval(dialog, "Remove saved node",
                                QString("Remove “%1” from the node library?").arg(entry["name"].toString()), "Remove")) return;
                        if (m_preview->removeNodeTemplate(entry["id"].toString())) dialog->reject();
                        else Ui::showNotice(dialog, "Saved nodes", "The saved node could not be removed.");
                    });
                    menu.exec(button->mapToGlobal(point));
                });
            }
            grid->setRowStretch((end - start + columns - 1) / columns, 1);
            pages->addWidget(page);
        }
        if (pages->count() > 1) {
            auto *navigationBar = new QWidget;
            auto *navigation = new QHBoxLayout(navigationBar);
            navigation->setContentsMargins(0, 0, 0, 0);
            auto *previous = action("Previous");
            auto *next = action("Next");
            auto *pageNumber = label({}, "cardMeta");
            navigation->addWidget(previous);
            navigation->addWidget(pageNumber, 1, Qt::AlignCenter);
            navigation->addWidget(next);
            footer->addWidget(navigationBar);
            navigationBar->hide();
            connect(tabs, &TabWidget::currentChanged, navigationBar, [navigationBar](int index) {
                navigationBar->setVisible(index == 1);
            });
            auto updateNavigation = [pages, previous, next, pageNumber](int index) {
                previous->setEnabled(index > 0);
                next->setEnabled(index + 1 < pages->count());
                pageNumber->setText(QString("Page %1 of %2").arg(index + 1).arg(pages->count()));
            };
            connect(pages, &QStackedWidget::currentChanged, dialog, updateNavigation);
            connect(previous, &Button::clicked, pages, [pages] { pages->setCurrentIndex(pages->currentIndex() - 1); });
            connect(next, &Button::clicked, pages, [pages] { pages->setCurrentIndex(pages->currentIndex() + 1); });
            updateNavigation(0);
        }
    }
    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
    footer->addStretch();
    footer->addWidget(buttons);
    body->addLayout(footer);
    connect(buttons, &QDialogButtonBox::rejected, dialog, &QDialog::reject);
    connect(dialog, &QDialog::accepted, this, [this, dialog, position] {
        auto node = dialog->property("savedNode").toJsonObject();
        if (node.isEmpty()) {
            const auto kind = dialog->property("nodeKind").toString();
            const auto configured = configureBlueprintNode(*m_preview, m_canvas, kind);
            if (!configured) return;
            node = *configured;
        } else {
            node["id"] = BlueprintPreview::id();
        }
        const auto size = BlueprintScene::nodeRect(node).size();
        const auto point = (position.value_or(m_canvas->centerInGraph()) - QPointF(size.width(), size.height()) / 2).toPoint();
        node["x"] = point.x();
        node["y"] = point.y();
        addNode(node);
    });
    dialog->setFixedWidth(width);
    dialog->resize(width, dialog->sizeHint().height());
    dialog->open();
}

void BlueprintPanel::addEntry(const BlueprintLibraryDock::Entry &entry, std::optional<QPointF> at) {
    auto node = entry.operation.isEmpty() ? entry.savedNode : m_preview->makeNode(entry.operation, BlueprintPreview::id(), 0, 0);
    if (node.isEmpty()) return;
    node["id"] = BlueprintPreview::id();
    QPointF point;
    if (at) point = *at;
    else {
        // The middle of the canvas the floating panels leave free.
        const auto size = BlueprintScene::nodeRect(node).size();
        point = m_canvas->mapToScene(m_canvas->freeArea().center()) - QPointF(size.width(), size.height()) / 2;
    }
    node["x"] = qRound(point.x());
    node["y"] = qRound(point.y());
    addNode(node);
}

void BlueprintPanel::configureNode(const QString &id) {
    const auto node = findById(m_graph["nodes"].toArray(), id);
    if (node.isEmpty()) return;
    const auto spec = m_preview->operation(BlueprintPreview::operationId(node));
    const auto configured = configureBlueprintNode(*m_preview, m_canvas, spec["kind"].toString(), node);
    if (configured && *configured != node) changeNode(*configured, true);
}

void BlueprintPanel::saveNodeTemplate(const QString &id) {
    const auto node = findById(m_graph["nodes"].toArray(), id);
    if (node.isEmpty()) return;
    bool accepted = false;
    const auto name = QInputDialog::getText(m_canvas, "Save to node library", "Name", QLineEdit::Normal,
        node["name"].toString(), &accepted).trimmed().left(60);
    if (!accepted || name.isEmpty()) return;
    if (!m_preview->saveNodeTemplate(name, node)) {
        Ui::showNotice(m_canvas, "Saved nodes", "The node could not be saved.");
        return;
    }
}

void BlueprintPanel::addNode(const QJsonObject &source) {
    auto graph = m_graph;
    auto nodes = graph["nodes"].toArray();
    auto node = source;
    auto bounds = BlueprintScene::nodeRect(node);
    const auto start = bounds.topLeft();
    qreal rowHeight = bounds.height();
    QList<QRectF> occupied;
    for (const auto &value : nodes) {
        const auto box = BlueprintScene::nodeRect(value.toObject());
        rowHeight = qMax(rowHeight, box.height());
        occupied.append(box.adjusted(-18, -18, 18, 18));
    }
    for (int slot = 1; std::any_of(occupied.cbegin(), occupied.cend(), [&](const auto &box) {
        return box.intersects(bounds);
    }); ++slot) {
        bounds.moveTopLeft(start + QPointF((slot % 3) * (bounds.width() + 28), (slot / 3) * (rowHeight + 32)));
    }
    node["x"] = qRound(bounds.x());
    node["y"] = qRound(bounds.y());
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
    if (!Ui::confirmRemoval(m_canvas, "Delete node",
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
    refreshInspector();
}

void BlueprintPanel::renameNode(const QString &id) {
    auto node = findById(m_graph["nodes"].toArray(), id);
    if (node.isEmpty()) return;
    bool accepted = false;
    const auto name = QInputDialog::getText(m_canvas, "Rename node", "Name", QLineEdit::Normal,
        node["name"].toString(), &accepted).trimmed().left(60);
    if (!accepted || name.isEmpty() || name == node["name"].toString()) return;
    node["name"] = name;
    changeNode(node);
}

void BlueprintPanel::showNodeMenu(const QString &id, const QPoint &screenPosition, const QPointF &) {
    const auto node = findById(m_graph["nodes"].toArray(), id);
    if (node.isEmpty()) return;
    QMenu menu(m_canvas);
    m_selected = id;
    refreshInspector();
    menu.addAction("Configure node…", this, [this, id] { configureNode(id); });
    menu.addAction("Rename…", this, [this, id] { renameNode(id); });
    // Wiring by dialog stays reachable from the keyboard here; drag-to-connect covers the mouse.
    menu.addAction("Connect ports…", this, [this, id] { connectPorts(id); })
        ->setEnabled(m_graph["nodes"].toArray().size() > 1);
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
    SettingsDetailDialog dialog(m_canvas);
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
    addPorts(out, "out", {}, {});
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
        const int row = existing->currentRow();
        const auto edges = m_graph["edges"].toArray();
        if (row < 0 || row >= edges.size()) return;
        if (removeConnection(edges.at(row).toObject())) refresh();
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
    return saveGraph(graph);
}

bool BlueprintPanel::dropConnection(const QJsonObject &edge) {
    return saveConnected(m_graph, edge);
}

bool BlueprintPanel::moveConnection(const QJsonObject &oldEdge, const QJsonObject &newEdge) {
    return saveConnected(withoutEdge(m_graph, oldEdge), newEdge);
}

// Connects edge into base and saves the result once. On an error, a declined prompt or a failed save
// nothing is stored and the canvas returns to the stored graph.
bool BlueprintPanel::saveConnected(const QJsonObject &base, const QJsonObject &edge) {
    int dropped = 0;
    QString error;
    const auto graph = m_preview->connectGraph(base, edge, &dropped, &error);
    if (graph.isEmpty()) {
        Ui::showNotice(m_canvas, "Connect ports", error);
        refreshEditor();
        return false;
    }
    if (dropped > 0 && !Ui::confirmAction(m_canvas, "Update node",
            QString("This change will remove %1.").arg(countOf(dropped, "incompatible connection")), "Save changes")) {
        refreshEditor();
        return false;
    }
    if (saveGraph(graph)) return true;
    refreshEditor();
    return false;
}

bool BlueprintPanel::removeConnection(const QJsonObject &edge) {
    bool found = false;
    const auto graph = withoutEdge(m_graph, edge, &found);
    if (!found) return false;
    if (saveGraph(graph)) return true;
    refreshEditor();
    return false;
}

void BlueprintPanel::editBinding() {
    if (m_preview->live()) { editLiveBinding(); return; }
    const auto node = findById(m_graph["nodes"].toArray(), m_selected);
    SettingsDetailDialog dialog(m_canvas);
    dialog.setWindowTitle("Model & adapter");
    auto *body = new QVBoxLayout(&dialog);
    titleBar(body, dialog.windowTitle());
    body->addWidget(wrapped("Choose a local binding for " + node["name"].toString() + "."));
    auto *fields = new BlueprintBindingFields(*m_preview, m_graph, node);
    body->addWidget(fields);
    auto *note = wrapped({}, "blueprintError");
    body->addWidget(note);
    auto *buttons = dialogButtons(&dialog, body, "Save binding");
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const auto error = fields->error();
        note->setText(error);
        if (error.isEmpty()) dialog.accept();
    });
    dialog.resize(450, dialog.sizeHint().height());
    if (dialog.exec() == QDialog::Accepted) changeNode(fields->node());
}

void BlueprintPanel::editLiveBinding() {
    const auto node = findById(m_graph["nodes"].toArray(), m_selected);
    if (node.isEmpty()) return;
    SettingsDetailDialog dialog(this);
    dialog.setWindowTitle(node["kind"] == "reasoning" ? "Reasoning checkpoint" : "Model & adapter");
    auto *body = new QVBoxLayout(&dialog);
    titleBar(body, dialog.windowTitle());
    auto *fields = new BlueprintBindingFields(*m_preview, m_graph, node);
    body->addWidget(fields);
    auto *note = wrapped({}, "blueprintError");
    body->addWidget(note);
    auto *buttons = dialogButtons(&dialog, body, "Save binding");
    auto *save = buttons->buttons().last();
    for (auto *button : buttons->buttons())
        if (buttons->buttonRole(button) == QDialogButtonBox::AcceptRole) save = button;
    save->setEnabled(fields->ready());
    connect(fields, &BlueprintBindingFields::readyChanged, save, &QWidget::setEnabled);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const auto error = fields->error();
        note->setText(error);
        if (error.isEmpty()) dialog.accept();
    });
    dialog.resize(460, dialog.sizeHint().height());
    if (dialog.exec() == QDialog::Accepted) changeNode(fields->node());
}

void BlueprintPanel::editDataset() {
    const auto node = findById(m_graph["nodes"].toArray(), m_selected);
    SettingsDetailDialog dialog(this);
    dialog.setWindowTitle("Dataset source");
    auto *body = new QVBoxLayout(&dialog);
    titleBar(body, dialog.windowTitle());
    auto *fields = new BlueprintDatasetFields(*m_preview, node);
    body->addWidget(fields);
    auto *note = wrapped({}, "blueprintError");
    body->addWidget(note);
    auto *buttons = dialogButtons(&dialog, body, "Save dataset");
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&] {
        const auto error = fields->error();
        note->setText(error);
        if (error.isEmpty()) dialog.accept();
    });
    dialog.resize(470, 340);
    if (dialog.exec() == QDialog::Accepted) changeNode(fields->node());
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
    auto *note = wrapped("Provide examples matching the named input and output ports. Follow queued jobs in Training.");
    const bool usesAdapterNode = !BlueprintPreview::adapterSource(graph, findById(graph["nodes"].toArray(), node)).isEmpty();
    if (usesAdapterNode)
        note->setText("Select completed adapter weights on the connected Adapter node.");
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
    connect(jobs, &ComboBox::currentIndexChanged, &dialog, [jobs, bind, usesAdapterNode] {
        bind->setEnabled(!usesAdapterNode && !bind->property("busy").toBool() && jobs->currentData(Qt::UserRole + 1).toBool());
    });
    connect(timer, &QTimer::timeout, &dialog, refresh);
    timer->start(2500);
    refresh();
    connect(buttons, &QDialogButtonBox::accepted, &dialog, [&, guard = QPointer(&dialog)] {
        const auto parsed = QJsonDocument::fromJson(examples->toPlainText().toUtf8());
        if (!parsed.isArray() || parsed.array().isEmpty()) { note->setText("Enter at least one typed example as a JSON array."); return; }
        pending(true);
        m_preview->train(graph, node, parsed.array(), QJsonObject{{"name", name->text().trimmed()}, {"target_modules", "all-linear"}},
            [guard, note, jobs, bind, refresh, pending, usesAdapterNode](const auto &job, const auto &error) {
                if (!guard) return;
                pending(false);
                bind->setEnabled(!usesAdapterNode && jobs->currentData(Qt::UserRole + 1).toBool());
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
    SettingsDetailDialog dialog(m_canvas);
    dialog.setWindowTitle("Train an adapter");
    auto *body = new QVBoxLayout(&dialog);
    titleBar(body, dialog.windowTitle());
    auto *name = new QLineEdit(existing["name"].toString(BlueprintPreview::operationId(node) == "selector" ? "new-order-selector" : "new-order-fields"));
    name->setMaxLength(48);
    auto *dataset = new ComboBox;
    dataset->addItems({"Training examples · fixture", "Small smoke-test dataset · fixture"});
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
    body->addWidget(wrapped("Supported keywords: type, properties, required, additionalProperties, items, "
                            "minimum (numbers) and minLength (text).", "cardMeta"));
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
    if (m_canvasPages->currentWidget() != m_canvas) return;
    // The view takes its overlays (library, inspector, dock, minimap and the top bar) with it.
    auto *dialog = new QDialog(this, Qt::Dialog | Qt::FramelessWindowHint);
    m_fullscreenDialog = dialog;
    dialog->setWindowTitle("Workflow canvas");
    dialog->setWindowModality(Qt::WindowModal);
    auto *layout = new QVBoxLayout(dialog);
    layout->setContentsMargins(0, 0, 0, 0);
    m_canvasPages->removeWidget(m_canvas);
    layout->addWidget(m_canvas);
    m_canvas->show();
    m_bar->setFullscreen(true);
    placeBar();
    connect(dialog, &QDialog::finished, this, &BlueprintPanel::leaveFullscreen);
    dialog->showFullScreen();
    m_canvas->setFocus();
}

void BlueprintPanel::leaveFullscreen() {
    auto *dialog = std::exchange(m_fullscreenDialog, nullptr);
    if (!dialog) return;
    dialog->layout()->removeWidget(m_canvas);
    m_canvasPages->insertWidget(0, m_canvas);
    m_canvasPages->setCurrentWidget(m_graph["nodes"].toArray().isEmpty() ? m_emptyCanvas : m_canvas);
    m_canvas->show();
    m_bar->setFullscreen(false);
    m_bar->setFullscreenAvailable(!m_graph["nodes"].toArray().isEmpty());
    placeBar();
    dialog->deleteLater();
    m_canvas->setFocus();
}

QWidget *BlueprintPanel::setupRun() {
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
    return page;
}

void BlueprintPanel::refreshRunOptions() {
    const auto target = m_target->currentData().toString();
    const bool nodeOnly = !target.isEmpty();
    if (m_preview->live()) {
        const auto contracts = BlueprintPreview::inputContracts(m_graph, target);
        m_sampleField->hide();
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
    showView(BlueprintTopBar::View::TestCases);
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
        m_preview->runLive(m_graph, input, target, false,
            [self = QPointer(this), nav](const auto &run, const auto &error) {
                if (!self) return;
                self->m_starting = false;
                self->refreshRunOptions();
                if (nav != self->m_navigation) return;
                self->m_runError->setText(error);
                self->m_runError->setVisible(!error.isEmpty());
                if (run.isEmpty()) return;
                self->m_runId = run["id"].toString();
                self->m_canGoBack = false;
                self->refreshBar();
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
        if (m_fullscreenDialog) m_fullscreenDialog->reject();
        m_graph = {};
        m_bar->setBlueprint(run["name"].toString(), m_preview->live() || graphId.isEmpty()
            ? "Inspect the run and its recorded steps." : "This run was recorded before its blueprint was deleted.",
            false, !m_preview->live());
        m_bar->setEditable(false);
        m_runSetup->hide();
        m_pages->setCurrentWidget(m_workspace);
    }
    showView(BlueprintTopBar::View::TestCases);
    m_runId = id;
    m_canGoBack = run["source"].toString() == "Local chat";
    refreshBar();
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
    // A step error is shown on its step; repeat it here only when no step carries it.
    const bool stepError = std::any_of(steps.begin(), steps.end(), [](const auto &value) {
        return !value.toObject()["error"].toString().isEmpty();
    });
    if (!runError.isEmpty() && !stepError) m_trace->addWidget(wrapped(runError, "blueprintError"));
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
    auto source = run["source"].toString();
    if (source == "Local chat") source = run["toolChat"].toBool() ? "Tool mode" : "Conversation";
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
    auto *runId = wrapped("Run ID  " + m_runId, "cardMeta");
    runId->setAccessibleName("Run ID: " + m_runId);
    runId->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_trace->addWidget(runId);
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
        const auto produced = step["output"];
        const bool empty = produced.isUndefined() || produced.isNull()
            || (produced.isObject() && produced.toObject().isEmpty());
        if (empty && status != "passed") {
            output->addWidget(label(outputTitle, "headingLabel"));
            output->addWidget(wrapped(status == "skipped" || status == "pending" ? "Not run"
                                      : "No output because the step did not finish.", "cardMeta"));
            output->addStretch();
        } else {
            fillCode(output, outputTitle, produced);
        }
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
        auto *meta = wrapped((rev ? QString("Revision %1").arg(rev) : QString("Draft node")) + " · " + id.left(8), "cardMeta");
        meta->setToolTip("Run ID " + id);
        title->addWidget(meta);
        row->addLayout(title, 1);
        row->setSpacing(16);
        // Fixed columns keep status, source, duration and time aligned down the list.
        const auto column = [row](QLabel *cell, int width) {
            cell->setFixedWidth(width);
            row->addWidget(cell);
        };
        column(statusLabel(run["status"].toString()), 118);
        auto source = run["source"].toString();
        if (source == "Local chat") source = run["toolChat"].toBool() ? "Tool mode" : "Conversation";
        column(label(source, "cardMeta"), 92);
        auto *elapsed = label(run.contains("ended")
            ? BlueprintPreview::duration(run["ended"].toInteger() - run["created"].toInteger()) : "—", "cardMeta");
        elapsed->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        column(elapsed, 56);
        column(label(QDateTime::fromMSecsSinceEpoch(run["created"].toInteger()).toString("d MMM, h:mm AP"), "cardMeta"), 112);
        auto *open = action("Inspect", false, true);
        row->addWidget(open);
        connect(open, &Button::clicked, this, [this, id] { openRun(id); });
        m_history->addLayout(row);
        m_history->addWidget(rule());
    }
    m_history->addStretch();
}

bool BlueprintPanel::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_canvas && event->type() == QEvent::Resize) applyCompactRule();
    return QWidget::eventFilter(watched, event);
}

void BlueprintPanel::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    if (m_dark == Theme::isDark()) return;
    m_dark = Theme::isDark();
    QTimer::singleShot(0, this, [this] {
        refreshEditor();
        refreshRun();
        if (m_bar->view() == BlueprintTopBar::View::RunHistory) refreshHistory();
    });
}

namespace {
bool isToolTask(const QJsonObject &run) {
    return run["toolChat"].toBool(!run["details"].toBool() && run["name"] == "Tool chat");
}

QLabel *resultText(const QJsonValue &value) {
    auto *text = wrapped(value.isString() ? value.toString() : BlueprintPreview::jsonText(value),
        value.isString() ? "blueprintMessage" : "blueprintCode");
    text->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    return text;
}

void addChatBubble(QVBoxLayout *layout, ChatBubble::Role role, const QString &text) {
    auto *bubble = new ChatBubble(role, text, layout->parentWidget());
    bubble->setWidthReference(layout->parentWidget());
    layout->addWidget(bubble, 0, role == ChatBubble::User ? Qt::AlignRight : Qt::AlignLeft);
}

QJsonArray taskMessages(const QJsonObject &previous, const QString &input) {
    auto messages = previous["messages"].toArray();
    if (!messages.isEmpty()) {
        if (previous["status"] == "passed" && previous["result"].isString())
            messages.append(QJsonObject{{"role", "assistant"}, {"content", previous["result"]}});
        else
            messages.removeLast();
    }
    qsizetype length = input.size();
    for (const auto &message : messages) length += message.toObject()["content"].toString().size();
    while (!messages.isEmpty() && (messages.size() >= 64 || length > 128000))
        length -= messages.takeAt(0).toObject()["content"].toString().size();
    while (!messages.isEmpty() && messages.first().toObject()["role"] != "user") messages.removeFirst();
    messages.append(QJsonObject{{"role", "user"}, {"content", input}});
    return messages;
}
}

namespace {
class ConversationRunLogo : public QWidget {
public:
    using QWidget::QWidget;
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        BrandMark::paint(painter, rect(), QColor(Theme::accentPrimary()));
    }
};

// The blank screen of a new task or workflow session, in the same voice as a new chat.
void addWelcome(QVBoxLayout *layout, const QString &eyebrow, const QString &title, const QString &hint) {
    layout->addStretch();
    auto *seal = new ConversationRunLogo;
    seal->setObjectName("conversationLogo");
    seal->setFixedSize(64, 64);
    seal->setAccessibleName("Magnite Space logo");
    layout->addWidget(seal, 0, Qt::AlignHCenter);
    layout->addSpacing(4);
    auto *marker = label(eyebrow, "chatEyebrow");
    marker->setAlignment(Qt::AlignCenter);
    layout->addWidget(marker);
    // Its own name (styled like the chat greeting) so lookups for the Conversation greeting stay unambiguous.
    auto *heading = wrapped(title, "blueprintRunHeading");
    heading->setAlignment(Qt::AlignCenter);
    layout->addWidget(heading);
    auto *detail = wrapped(hint);
    detail->setAlignment(Qt::AlignCenter);
    layout->addWidget(detail);
    layout->addStretch();
}
}

ConversationRunPanel::ConversationRunPanel(BlueprintPreview *preview, Mode mode, QWidget *parent)
    : QWidget(parent), m_preview(preview), m_mode(mode), m_dark(Theme::isDark()) {
    const bool tool = mode == Mode::Tool;
    setObjectName(tool ? "conversationToolMode" : "conversationBlueprintMode");
    auto *outer = new QHBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto *column = new QWidget(this);
    column->setMaximumWidth(840);
    outer->addStretch();
    outer->addWidget(column, 1);
    outer->addStretch();
    auto *layout = new QVBoxLayout(column);
    layout->setContentsMargins(12, 4, 12, 4);
    layout->setSpacing(10);

    auto *header = new QHBoxLayout;
    // Both modes keep a session: the runs on screen. New starts an empty one; history reopens a saved run.
    auto *history = action(tool ? "Task history" : "Run history", false, true);
    m_newTask = action(tool ? "New task" : "New run");
    m_newTask->setIcon(Theme::coloredIcon("plus", QColor(Theme::textSecondary()), 16));
    m_newTask->setProperty("themeIcon", "plus");
    connect(history, &Button::clicked, this, &ConversationRunPanel::openTaskHistory);
    connect(this, &ConversationRunPanel::activityChanged, history, [this, history] {
        history->setEnabled(!hasActiveTask());
    });
    connect(m_newTask, &Button::clicked, this, [this] {
        m_taskRuns.clear();
        m_input->clear();
        m_error->hide();
        rebuildTranscript();
        updateActions();
        if (m_input->isVisible()) m_input->setFocus();
        else if (m_blueprint) m_blueprint->setFocus();
    });
    if (tool) {
        header->addWidget(label("Task conversation", "headingLabel"), 1);
        header->addWidget(history);
        header->addWidget(m_newTask);
    } else {
        header->addWidget(label("Workflow", "headingLabel"));
        m_blueprint = new ComboBox;
        m_blueprint->setAccessibleName("Saved blueprint");
        m_blueprint->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_blueprint->setMinimumContentsLength(16);
        header->addWidget(m_blueprint, 1);
        auto *library = action("Open Blueprints", false, true);
        header->addWidget(library);
        header->addWidget(history);
        header->addWidget(m_newTask);
        connect(library, &Button::clicked, this, &ConversationRunPanel::libraryRequested);
        connect(m_blueprint, &ComboBox::currentIndexChanged, this, [this] {
            m_preferredBlueprint.clear();
            m_error->hide();
            refreshComposer();
        });
        connect(m_preview, &BlueprintPreview::savedChanged, this, &ConversationRunPanel::refreshPicker);
    }
    layout->addLayout(header);
    layout->addWidget(rule());

    m_scroll = scroller(&m_messages);
    m_messages->setContentsMargins(0, 8, 6, 8);
    m_messages->setSpacing(12);
    layout->addWidget(m_scroll, 1);
    auto *composer = Surface::card(this);
    composer->parentWidget()->setObjectName(tool ? "toolTaskComposer" : "workflowComposer");
    m_input = new QPlainTextEdit;
    m_input->setObjectName(tool ? "toolTaskInput" : "workflowInput");
    m_input->setAccessibleName(tool ? "Task message" : "Workflow inputs");
    m_input->setTabChangesFocus(true);
    m_input->setFixedHeight(tool ? 74 : 86);
    if (tool) m_input->installEventFilter(this);
    composer->addWidget(m_input);
    m_error = wrapped({}, "blueprintError");
    m_error->hide();
    composer->addWidget(m_error);
    auto *actions = new QHBoxLayout;
    m_inputHint = wrapped({});
    actions->addWidget(m_inputHint, 1);
    if (tool) {
        m_stop = action("Stop");
        m_stop->hide();
        actions->addWidget(m_stop);
        connect(m_stop, &Button::clicked, this, [this] {
            if (!m_taskRuns.isEmpty()) m_preview->cancel(m_taskRuns.last());
        });
    }
    m_send = action(tool ? "Send ↑" : "Run workflow", true);
    actions->addWidget(m_send);
    composer->addLayout(actions);
    layout->addWidget(composer->parentWidget());

    connect(m_send, &Button::clicked, this, &ConversationRunPanel::send);
    connect(m_input, &QPlainTextEdit::textChanged, this, &ConversationRunPanel::updateActions);
    if (!tool) {
        auto *sendKey = new QShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return), m_input);
        sendKey->setContext(Qt::WidgetShortcut);
        connect(sendKey, &QShortcut::activated, this, &ConversationRunPanel::send);
    }
    connect(m_preview, &BlueprintPreview::runChanged, this, [this, tool](const QString &id) {
        const auto run = m_preview->run(id);
        if (run["source"] != "Local chat" || tool != isToolTask(run)) return;
        // A task still running from before (e.g. after a restart) resumes in Tool mode. A workflow session only
        // holds runs started or reopened here, so New run stays empty while an earlier run finishes.
        if (tool && m_taskRuns.isEmpty() && !m_starting && running(run)) m_taskRuns.append(id);
        if (m_taskRuns.contains(id)) {
            rebuildTranscript();
            updateActions();
            if (tool) emit activityChanged();
        }
    });
    connect(m_preview, &BlueprintPreview::serviceChanged, this, [this, tool] {
        const bool connected = m_preview->connected();
        if (connected != m_connected || !tool) refreshComposer();
        m_connected = connected;
        rebuildTranscript();
        updateActions();
    });
    m_connected = m_preview->connected();
    if (tool) refreshComposer();
    else refreshPicker();
    rebuildTranscript();
}

bool ConversationRunPanel::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_input && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter)
            && !(key->modifiers() & Qt::ShiftModifier)
            && (QSettings().value("chat/sendKey", "enter").toString() != "modified"
                || (key->modifiers() & Qt::ControlModifier))) {
            if (!key->isAutoRepeat()) send();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ConversationRunPanel::activate(const QString &blueprint) {
    if (m_mode == Mode::Blueprint) {
        if (!blueprint.isEmpty()) m_preferredBlueprint = blueprint;
        refreshPicker();
        m_preview->refreshSaved();
        m_blueprint->setFocus();
    } else {
        refreshComposer();
        m_input->setFocus();
    }
}

void ConversationRunPanel::modelChanged(bool changing) {
    m_modelChanging = changing;
    if (isVisible()) refreshComposer();
}

bool ConversationRunPanel::hasActiveTask() const {
    return m_mode == Mode::Tool && (m_starting ||
        (!m_taskRuns.isEmpty() && running(m_preview->run(m_taskRuns.last()))));
}

void ConversationRunPanel::refreshPicker() {
    if (!m_blueprint) return;
    const auto selected = m_preferredBlueprint.isEmpty() ? m_blueprint->currentData().toString() : m_preferredBlueprint;
    const auto choices = m_preview->live() ? m_preview->published() : m_preview->saved();
    {
        const QSignalBlocker block(m_blueprint);
        m_blueprint->clear();
        m_blueprint->addItem(choices.isEmpty() ? "No saved revisions" : "Choose a blueprint");
        for (const auto &value : choices) {
            const auto graph = value.toObject();
            const auto revision = graph["revision"].toInteger();
            const auto name = graph["name"].toString();
            m_blueprint->addItem(QString("%1 · Rev %2").arg(name).arg(revision), graph["id"].toString());
            m_blueprint->setItemData(m_blueprint->count() - 1, revision, Qt::UserRole + 1);
        }
        const int match = m_blueprint->findData(selected);
        if (match > 0) m_preferredBlueprint.clear();
        m_blueprint->setCurrentIndex(match > 0 ? match : choices.size() == 1 && m_preferredBlueprint.isEmpty() ? 1 : 0);
        m_blueprint->setEnabled(!choices.isEmpty());
        m_blueprint->setToolTip(m_blueprint->currentText());
    }
    refreshComposer();
}

void ConversationRunPanel::refreshComposer() {
    const auto request = ++m_selection;
    m_readyToSend = false;
    const bool tool = m_mode == Mode::Tool;
    if (m_preview->live() && !m_preview->connected()) {
        m_chatGraph = {};
        m_inputHint->setText("Waiting for the local service.");
        updateActions();
        return;
    }
    if (!tool) {
        const auto id = m_blueprint->currentData().toString();
        const auto revision = m_blueprint->currentData(Qt::UserRole + 1).toLongLong();
        if (!id.isEmpty() && m_chatGraph["id"] == id && m_chatGraph["revision"].toInteger() == revision) {
            m_readyToSend = true;
            updateActions();
            return;
        }
        m_chatGraph = {};
        m_blueprint->setToolTip(m_blueprint->currentText());
        m_input->hide();
        updateActions();
        if (id.isEmpty()) {
            QString hint = "Choose a saved workflow to see its inputs.";
            if (!m_preview->serviceError().isEmpty()) hint = m_preview->serviceError();
            else if (m_blueprint->count() == 1) hint = m_preview->saved().isEmpty()
                ? "Create a workflow in Blueprints, then choose Save revision."
                : "Open your draft in Blueprints and choose Save revision to make it available here.";
            m_inputHint->setText(hint);
            return;
        }
        const auto apply = [self = QPointer(this), request](const QJsonObject &graph, const QString &error) {
            if (!self || self->m_selection != request) return;
            self->m_chatGraph = graph;
            const auto contracts = BlueprintPreview::inputContracts(graph, {});
            self->m_input->setVisible(error.isEmpty() && !contracts.isEmpty());
            const bool text = contracts.size() == 1 && contracts.begin().value().toObject()["type"] == "string";
            self->m_input->setPlaceholderText(text ? "Enter text…" : "Enter JSON…");
            self->m_inputHint->setText(error.isEmpty() ? inputHint(contracts, false) : error);
            self->m_readyToSend = error.isEmpty();
            self->updateActions();
        };
        if (!m_preview->live()) {
            apply(m_preview->blueprint(id), {});
            return;
        }
        m_inputHint->setText("Reading the saved revision…");
        m_preview->getBlueprint(id, true, apply, revision);
        return;
    }
    updateActions();
    if (m_modelChanging) {
        m_inputHint->setText("Updating the loaded model…");
        return;
    }
    if (!m_preview->live()) {
        m_inputHint->setText("Fixture data · Actions require approval");
        m_readyToSend = true;
        updateActions();
        return;
    }
    m_inputHint->setText("Checking the local model and Pi harness…");
    m_preview->toolCapability([self = QPointer(this), request](const auto &capability, const auto &error) {
        if (!self || self->m_selection != request) return;
        self->m_readyToSend = error.isEmpty();
        self->m_inputHint->setText(error.isEmpty()
            ? capability["label"].toString(BlueprintPreview::modelLabel(capability["model"].toString()))
                + " · You approve each action before it runs"
            : error);
        self->updateActions();
    });
}

void ConversationRunPanel::updateActions() {
    const bool text = !m_input->toPlainText().trimmed().isEmpty();
    const bool tool = m_mode == Mode::Tool;
    const bool active = hasActiveTask();
    const bool needsInput = tool || !BlueprintPreview::inputContracts(m_chatGraph, {}).isEmpty();
    m_send->setEnabled(m_readyToSend && !m_starting && !active && (!needsInput || text));
    m_newTask->setEnabled(!active && (!m_taskRuns.isEmpty() || text));
    if (tool) {
        m_stop->setVisible(active && !m_starting);
        m_stop->setEnabled(!m_preview->live() || m_preview->connected());
        m_input->setPlaceholderText(m_taskRuns.isEmpty() ? "Describe a task for your local model…" : "Ask a follow-up…");
    }
}

void ConversationRunPanel::send() {
    if (!m_send->isEnabled()) return;
    const bool tool = m_mode == Mode::Tool;
    const auto original = m_input->toPlainText();
    QString error;
    m_error->hide();
    if (!m_preview->live()) {
        const auto scenario = !tool ? "normal" :
            original.contains("version", Qt::CaseInsensitive) ? "command" : "approval";
        const auto id = m_preview->start(m_chatGraph, original, {}, scenario, "Local chat", &error);
        if (!id.isEmpty() && !m_taskRuns.contains(id)) m_taskRuns.append(id);
        if (error.isEmpty()) m_input->clear();
        m_error->setText(error);
        m_error->setVisible(!error.isEmpty());
        rebuildTranscript();
        updateActions();
        return;
    }

    const auto input = tool ? QJsonObject{} :
        collectInput(BlueprintPreview::inputContracts(m_chatGraph, {}), original, &error);
    if (tool && original.size() > 128000) error = "A task message can contain up to 128,000 characters.";
    if (!error.isEmpty()) {
        m_error->setText(error);
        m_error->show();
        return;
    }

    m_starting = true;
    updateActions();
    if (tool) emit activityChanged();
    const auto done = [self = QPointer(this), original, tool](const auto &run, const auto &error) {
        if (!self) return;
        self->m_starting = false;
        if (!run.isEmpty() && !self->m_taskRuns.contains(run["id"].toString())) self->m_taskRuns.append(run["id"].toString());
        if (error.isEmpty() && self->m_input->toPlainText() == original) self->m_input->clear();
        self->m_error->setText(error);
        self->m_error->setVisible(!error.isEmpty());
        self->rebuildTranscript();
        self->updateActions();
        if (tool) emit self->activityChanged();
        if (!run.isEmpty()) QTimer::singleShot(0, self, [self] {
            auto *bar = self->m_scroll->verticalScrollBar();
            bar->setValue(bar->maximum());
        });
    };
    if (tool) {
        const auto previous = m_taskRuns.isEmpty() ? QJsonObject{} : m_preview->run(m_taskRuns.last());
        m_preview->runToolTask(taskMessages(previous, original), done);
    } else {
        m_preview->runLive(m_chatGraph, input, {}, true, done);
    }
}

void ConversationRunPanel::openTaskHistory() {
    const bool tool = m_mode == Mode::Tool;
    SettingsDetailDialog dialog(this);
    dialog.setWindowTitle(tool ? "Task history" : "Run history");
    auto *body = new QVBoxLayout(&dialog);
    titleBar(body, dialog.windowTitle());
    auto *items = new QListWidget;
    items->setAccessibleName(tool ? "Recent tool tasks" : "Recent workflow runs");
    const auto runs = m_preview->chatRuns();
    for (auto it = runs.constEnd(); it != runs.constBegin();) {
        const auto run = (*--it).toObject();
        if (isToolTask(run) != tool) continue;
        const auto prompt = run["input"].toString().simplified();
        QString summary = prompt.isEmpty() ? run["name"].toString() : prompt.left(85);
        if (!tool) summary = run["name"].toString() + (prompt.isEmpty() ? QString() : " · " + prompt.left(60));
        auto *item = new QListWidgetItem(summary + " · " + BlueprintPreview::statusLabel(run["status"].toString()), items);
        item->setData(Qt::UserRole, run["id"]);
    }
    if (!items->count()) body->addWidget(wrapped(tool ? "Your recent tasks will appear here."
                                                      : "Workflows you run here will appear in this list."));
    body->addWidget(items);
    items->setCurrentRow(0);
    const auto buttons = dialogButtons(&dialog, body, tool ? "Open task" : "Open run");
    for (auto *button : buttons->buttons())
        if (buttons->buttonRole(button) == QDialogButtonBox::AcceptRole) button->setEnabled(items->count() > 0);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(items, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);
    dialog.resize(500, 340);
    if (dialog.exec() != QDialog::Accepted || !items->currentItem()) return;
    const auto id = items->currentItem()->data(Qt::UserRole).toString();
    if (m_taskRuns.contains(id) && m_taskRuns.last() == id) return;
    m_taskRuns = {id};
    m_input->clear();
    m_error->hide();
    if (!tool) {   // select the run's workflow, so running it again is one step
        const auto blueprint = m_preview->run(id)["blueprintId"].toString();
        if (!blueprint.isEmpty()) {
            m_preferredBlueprint = blueprint;
            refreshPicker();
        }
    }
    rebuildTranscript();
    updateActions();
    emit activityChanged();
    if (m_preview->live() && !m_preview->run(id)["details"].toBool()) m_preview->loadRun(id);
}

void ConversationRunPanel::rebuildTranscript() {
    auto *bar = m_scroll->verticalScrollBar();
    const bool follow = bar->value() >= bar->maximum() - 20;
    const auto *focus = QApplication::focusWidget();
    const auto focusedApproval = focus ? focus->property("approvalFocus").toString() : QString{};
    clear(m_messages);
    const bool tool = m_mode == Mode::Tool;
    QJsonArray runs;
    for (const auto &id : std::as_const(m_taskRuns)) {
        const auto run = m_preview->run(id);
        if (!run.isEmpty()) runs.append(run);
    }
    if (runs.isEmpty()) {
        if (tool)
            addWelcome(m_messages, "TOOL MODE", "What would you like to do?",
                       "Choose a local model and describe the task. You can review each action before it runs.");
        else
            addWelcome(m_messages, "BLUEPRINT", "Run a saved workflow",
                       "Choose a blueprint above to see its inputs. Results and tool approvals appear here.");
        return;
    }

    if (tool) {
        const auto first = runs.first().toObject()["messages"].toArray();
        for (int index = 0; index + 1 < first.size(); ++index) {
            const auto message = first.at(index).toObject();
            if (message["role"] == "user" || message["role"] == "assistant")
                addChatBubble(m_messages, message["role"] == "user" ? ChatBubble::User : ChatBubble::Assistant,
                    message["content"].toString());
        }
    }
    for (const auto &value : runs) {
        const auto run = value.toObject();
        const auto status = run["status"].toString();
        const auto id = run["id"].toString();
        QVBoxLayout *body = m_messages;
        if (tool) {
            if (run["input"].isString()) addChatBubble(body, ChatBubble::User, run["input"].toString());
        } else {
            body = Surface::card(this);
            body->parentWidget()->setObjectName("conversationWorkflowRun");
            auto *head = new QHBoxLayout;
            head->addWidget(wrapped(run["name"].toString(), "headingLabel"), 1);
            head->addWidget(label(QString("Rev %1").arg(run["revision"].toInteger()), "cardMeta"));
            head->addWidget(statusLabel(status));
            body->addLayout(head);
            if (run["input"].isString()) {
                body->addWidget(label("Submitted inputs", "cardMeta"));
                const auto supplied = run["supplied"].toObject();
                const auto single = supplied.size() == 1 ? supplied.begin().value() : QJsonValue{};
                body->addWidget(resultText(supplied.isEmpty() || single.isString() ? run["input"] : run["supplied"]));
            }
        }

        const auto steps = run["steps"].toArray();
        const auto cursor = run["cursor"].toInt(-1);
        const auto step = cursor >= 0 && cursor < steps.size() ? steps.at(cursor).toObject() : QJsonObject{};
        if (running(run)) {
            if (tool && status != "waiting") {
                body->addWidget(label(status == "queued" ? "Task queued" : "Working on it…", "blueprintRunning"));
            } else if (!tool && !steps.isEmpty()) {
                const auto passed = std::count_if(steps.cbegin(), steps.cend(),
                    [](const auto &value) { return value.toObject()["status"] == "passed"; });
                auto *progress = new QProgressBar;
                progress->setObjectName("conversationWorkflowProgress");
                progress->setAccessibleName("Workflow progress");
                progress->setRange(0, steps.size());
                progress->setValue(passed);
                progress->setTextVisible(false);
                progress->setFixedHeight(6);
                const auto count = QString("%1 of %2 steps completed").arg(passed).arg(steps.size());
                const auto stepName = step["name"].toString();
                body->addWidget(wrapped(stepName.isEmpty() ? count : count + " · " + stepName));
                body->addWidget(progress);
            }
        }
        const auto decisions = decisionsFor(run);
        for (const auto &decision : decisions) body->addWidget(approvalView(m_preview, run, decision.toObject()));

        if (status == "passed") {
            if (tool && run["result"].isString()) {
                const auto model = run["model"].toString();
                body->addWidget(wrapped(model.isEmpty() ? "Local model" : model, "cardMeta"));
                addChatBubble(body, ChatBubble::Assistant, run["result"].toString());
            } else if (!run["result"].isUndefined()) {
                body->addWidget(label("Result", "headingLabel"));
                body->addWidget(resultText(run["result"]));
            } else if (!m_preview->live() && !steps.isEmpty() && !tool) {
                body->addWidget(resultText(steps.last().toObject()["output"]));
            }
        } else if (status == "failed") {
            body->addWidget(wrapped(run["error"].toString(step["error"].toString("The run failed.")), "blueprintError"));
        } else if (tool && status == "cancelled") {
            body->addWidget(label("Task stopped", "cardMeta"));
        }
        if (run["actionError"].isString()) body->addWidget(wrapped(run["actionError"].toString(), "blueprintError"));
        if (!tool || !decisions.isEmpty() || status == "failed") {
            auto *actions = new QHBoxLayout;
            auto *open = action(tool ? "View trace" : "Open full run", false, true);
            connect(open, &Button::clicked, this, [this, id] { emit runRequested(id); });
            actions->addWidget(open);
            if (!tool && running(run)) {
                auto *cancel = action("Cancel run", false, true);
                cancel->setEnabled(!m_preview->live() || m_preview->connected());
                connect(cancel, &Button::clicked, this, [this, id] { m_preview->cancel(id); });
                actions->addWidget(cancel);
            }
            actions->addStretch();
            body->addLayout(actions);
        }
        if (!tool) m_messages->addWidget(body->parentWidget());
    }

    m_messages->addStretch();
    if (!focusedApproval.isEmpty()) {
        for (auto *button : m_messages->parentWidget()->findChildren<Button *>())
            if (button->isVisible() && button->property("approvalFocus").toString() == focusedApproval) button->setFocus();
    }
    if (follow) QTimer::singleShot(0, m_scroll, [bar] { bar->setValue(bar->maximum()); });
}

void ConversationRunPanel::changeEvent(QEvent *event) {
    QWidget::changeEvent(event);
    if (m_dark == Theme::isDark()) return;
    m_dark = Theme::isDark();
    QTimer::singleShot(0, this, &ConversationRunPanel::rebuildTranscript);
}
