#include "panels/librarypanel.h"

#include "backend_client.h"
#include "panels/adaptercard.h"
#include "panels/chatstore.h"
#include "panels/adapterspanel.h"
#include "panels/modelcard.h"
#include "panels/modelspanel.h"
#include "theme.h"
#include "widgets/edgescrollbar.h"
#include "widgets/button.h"
#include "widgets/combobox.h"
#include "widgets/librarydialog.h"
#include "widgets/progressbar.h"
#include "widgets/surface.h"
#include "widgets/tabwidget.h"

#include <algorithm>
#include <QCheckBox>
#include <QDateTime>
#include <QHBoxLayout>
#include <QDesktopServices>
#include <QPlainTextEdit>
#include <QStyle>
#include <QTimer>
#include <QUrl>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QScrollArea>
#include <QPointer>
#include <QSettings>
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace {
enum Page { Models, Adapters, Projects, Folders, History };

QLabel *text(const QString &value, const char *style, QWidget *parent) {
    auto *label = new QLabel(value, parent);
    label->setObjectName(style);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    return label;
}

QVBoxLayout *scrollPage(TabWidget *tabs, const QString &title) {
    auto *scroll = new QScrollArea(tabs);
    scroll->setObjectName("cardScroll");
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    auto *body = new QWidget;
    auto *layout = new QVBoxLayout(body);
    layout->setContentsMargins(0, 12, 8, 0);
    layout->setAlignment(Qt::AlignTop);
    layout->setSpacing(10);
    scroll->setWidget(body);
    EdgeScrollBar::attach(scroll);
    tabs->addTab(scroll, title);
    return layout;
}

void clear(QVBoxLayout *layout) {
    while (auto *item = layout->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}
}

LibraryPanel::LibraryPanel(ModelsPanel *models, AdaptersPanel *adapters)
    : BasePanel("Library", nullptr, true), m_models(models), m_adapters(adapters) {
    const auto saved = QJsonDocument::fromJson(QSettings().value("library/organization").toByteArray()).object();
    m_projectNames = saved["projects"].toArray();
    m_folderNames = saved["folders"].toArray();
    m_memberships = saved["items"].toObject();
    m_events = saved["history"].toArray();
    m_collectionInfo = saved["collections"].toObject();

    auto *header = new QHBoxLayout;
    auto *identity = new QVBoxLayout;
    identity->addWidget(text("Library", "hubPageTitle", this));
    identity->addWidget(text("Organize local models and adapters by project and folder.", "panelSubtitle", this));
    header->addLayout(identity, 1);
    m_action = new Button(this);
    m_action->setProperty("primary", true);
    m_action->setMinimumWidth(174);
    header->addWidget(m_action);
    contentLayout()->addLayout(header);
    auto *storage = Surface::card(this);
    storage->setObjectName("libraryStorageLayout");
    m_storage = text({}, "hubMeta", storage->parentWidget());
    auto *storageRow = new QHBoxLayout;
    storageRow->addWidget(m_storage, 1);
    auto *disk = new QVBoxLayout;
    disk->setSpacing(4);
    m_diskUsage = text({}, "hubSmallMeta", storage->parentWidget());
    m_meter = new ProgressBar(storage->parentWidget());
    m_meter->setAccessibleName("Workspace disk usage");
    m_meter->setTextVisible(false);
    m_meter->setFixedHeight(5);
    m_meter->setMinimumWidth(220);
    m_meter->hide();
    disk->addWidget(m_diskUsage);
    disk->addWidget(m_meter);
    storageRow->addLayout(disk);
    storage->addLayout(storageRow);
    contentLayout()->addWidget(storage->parentWidget());

    auto *tools = new QHBoxLayout;
    m_scopeLabel = text({}, "hubSectionTitle", this);
    auto *scopeRow = new QHBoxLayout;
    scopeRow->addWidget(m_scopeLabel, 1);
    auto *all = new Button("All items", this);
    scopeRow->addWidget(all);
    m_pinned = new Button("Pinned", this);
    m_pinned->setCheckable(true);
    scopeRow->addWidget(m_pinned);
    m_search = new QLineEdit(this);
    m_search->setAccessibleName("Search Library");
    m_search->setClearButtonEnabled(true);
    m_search->addAction(Theme::coloredIcon("search", QColor(Theme::textMuted()), 16), QLineEdit::LeadingPosition);
    tools->addWidget(m_search, 1);
    m_filter = new ComboBox(this);
    m_filter->setAccessibleName("Filter Library");
    tools->addWidget(m_filter);
    m_sort = new ComboBox(this);
    m_sort->setAccessibleName("Sort Library");
    tools->addWidget(m_sort);
    auto *refresh = new Button("Refresh", this);
    tools->addWidget(refresh);
    connect(refresh, &QPushButton::clicked, this, [this] {
        if (m_tabs->currentIndex() == Models) m_models->refresh();
        if (m_tabs->currentIndex() == Adapters) m_adapters->refresh();
        refreshStorage();
    });
    auto *toolbar = new QWidget(this);
    tools->setContentsMargins(0, 0, 0, 0);
    m_results = text({}, "hubMeta", toolbar);
    auto *toolbarLayout = new QVBoxLayout(toolbar);
    toolbarLayout->setContentsMargins(0, 8, 0, 0);
    toolbarLayout->addLayout(scopeRow);
    toolbarLayout->addLayout(tools);
    toolbarLayout->addWidget(m_results);

    m_tabs = new TabWidget(this);
    m_tabs->setObjectName("libraryTabs");
    m_tabs->setDocumentMode(true);
    m_tabs->tabBar()->setProperty("underline", true);
    const auto inventoryPage = [this](QWidget *panel, const QString &name) {
        auto *page = new QWidget(m_tabs);
        auto *layout = new QVBoxLayout(page);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->addWidget(panel, 1);
        m_tabs->addTab(page, name);
        return layout;
    };
    auto *modelsPage = inventoryPage(m_models, "Models");
    auto *adaptersPage = inventoryPage(m_adapters, "Adapters");
    m_projects = scrollPage(m_tabs, "Projects");
    m_folders = scrollPage(m_tabs, "Folders");
    m_history = scrollPage(m_tabs, "History");
    auto *historyScroll = m_tabs->widget(History);
    m_tabs->removeTab(History);
    auto *historyPage = inventoryPage(historyScroll, "History");
    historyScroll->show();
    modelsPage->insertWidget(0, toolbar);
    contentLayout()->addWidget(m_tabs, 1);

    connect(m_tabs, &QTabWidget::currentChanged, this, [this, toolbar, all, refresh, modelsPage, adaptersPage, historyPage] {
        const int page = m_tabs->currentIndex();
        if (page == Models) modelsPage->insertWidget(0, toolbar);
        else if (page == Adapters) adaptersPage->insertWidget(0, toolbar);
        else if (page == History) historyPage->insertWidget(0, toolbar);
        toolbar->setVisible(page <= Adapters || page == History);
        const QSignalBlocker filterBlock(m_filter), sortBlock(m_sort);
        m_filter->clear();
        m_sort->clear();
        if (page == History) {
            m_filter->addItems({"All statuses", "Completed", "Failed", "Cancelled"});
            m_sort->addItems({"Newest first", "Oldest first"});
        } else {
            m_filter->addItems(page == Models ? QStringList{"All sources", "Downloaded", "Imported"}
                                              : QStringList{"All adapters"});
            m_sort->addItems({"Recently added", "Name A–Z", "Largest first"});
        }
        m_filter->setVisible(page != Adapters);
        all->setVisible(page <= Adapters);
        refresh->setVisible(page <= Adapters);
        m_pinned->setVisible(page <= Adapters);
        m_scopeLabel->setVisible(page <= Adapters);
        const QSignalBlocker block(m_search);
        m_search->clear();
        updateView();
    });
    connect(all, &QPushButton::clicked, this, [this] {
        m_scope.clear();
        m_pinned->setChecked(false);
        updateView();
    });
    connect(m_pinned, &QPushButton::toggled, this, &LibraryPanel::filterItems);
    connect(m_search, &QLineEdit::textChanged, this, [this] {
        if (m_tabs->currentIndex() == History) renderHistory();
        else filterItems();
    });
    connect(m_action, &QPushButton::clicked, this, [this] {
        switch (m_tabs->currentIndex()) {
        case Models: m_models->importModel(); break;
        case Adapters: m_adapters->onImportAdapter(); break;
        case Projects:
        case Folders:
            if ((m_tabs->currentIndex() == Folders ? m_openFolder : m_openProject).isEmpty()) createCollection();
            else addFiles(m_tabs->currentIndex() == Folders);
            break;
        }
    });
    connect(models, &ModelsPanel::modelsChanged, this, &LibraryPanel::updateView);
    connect(adapters, &AdaptersPanel::registryRefreshed, this, &LibraryPanel::updateView);
    m_filter->addItems({"All sources", "Downloaded", "Imported"});
    m_sort->addItems({"Recently added", "Name A–Z", "Largest first"});
    const auto applyControls = [this] {
        if (m_tabs->currentIndex() == History) renderHistory();
        else filterItems();
    };
    connect(m_filter, &QComboBox::currentIndexChanged, this, applyControls);
    connect(m_sort, &QComboBox::currentIndexChanged, this, applyControls);
    updateView();
}

void LibraryPanel::setBackendClient(BackendClient *client) {
    m_client = client;
    refreshStorage();
}

void LibraryPanel::refreshStorage() {
    if (!m_client || !m_client->isReady()) return;
    const QPointer<LibraryPanel> guard(this);
    m_client->get("/system/storage", [guard](int status, const QJsonDocument &document) {
        if (!guard || status != 200) return;
        const auto values = document.object();
        const double total = values["disk_total_bytes"].toDouble();
        const double free = values["free_bytes"].toDouble(-1);
        if (total <= 0 || free < 0 || free > total) return;
        guard->m_diskUsage->setText(QString("Workspace disk · %1 of %2 used")
            .arg(ModelCard::formatBytes(total - free), ModelCard::formatBytes(total)));
        guard->m_meter->setValue(qRound(100 * (total - free) / total));
        guard->m_meter->show();
    });
}

void LibraryPanel::setChatStore(ChatStore *store) {
    if (m_chatStore) disconnect(m_chatStore, nullptr, this, nullptr);
    m_chatStore = store;
    if (store) connect(store, &ChatStore::changed, this, [this] {
        if (m_tabs->currentIndex() == Projects || m_tabs->currentIndex() == Folders) renderCollections();
    }, Qt::QueuedConnection);
}

void LibraryPanel::showAdapters() { m_tabs->setCurrentIndex(Adapters); }

void LibraryPanel::updateView() {
    const int page = m_tabs->currentIndex();
    m_action->setVisible(page != History);
    const QString iconName = page <= Adapters ? "file-up" : "plus";
    m_action->setProperty("themeIcon", iconName);
    m_action->setIcon(Theme::coloredIcon(iconName, QColor(Theme::accentText()), 16));
    const QStringList actions{"Import model…", "Import adapter…", "New project…", "New folder…"};
    if (page != History) {
        const bool inside = (page == Projects && !m_openProject.isEmpty()) || (page == Folders && !m_openFolder.isEmpty());
        m_action->setText(inside ? "Add files…" : actions.at(page));
    }
    m_scopeLabel->setText(m_scope.isEmpty() ? (page == Adapters ? "All adapters" : "All models") : m_scope);
    m_search->setPlaceholderText(page == History ? "Search library activity…" :
                                 page == Adapters ? "Search adapters or base models…" : "Search local models…");
    const auto models = m_models->findChildren<ModelCard *>();
    const auto adapters = m_adapters->findChildren<AdapterCard *>();
    double size = 0;
    for (auto *card : models) size += card->data().value("size_mb").toDouble();
    for (auto *card : adapters) size += card->data().value("size_mb").toDouble();
    m_storage->setText(QString("Model storage   ·   %1 models   ·   %2 adapters   ·   %3 in local weights")
                      .arg(models.size()).arg(adapters.size()).arg(ModelCard::formatSizeMb(size)));
    filterItems();
    if (page == Projects || page == Folders) renderCollections();
    if (page == History) renderHistory();
}

void LibraryPanel::filterItems() {
    const bool adapters = m_tabs->currentIndex() == Adapters;
    if (m_tabs->currentIndex() > Adapters) return;
    const auto query = m_search->text().trimmed();
    auto apply = [this, &query](QWidget *card, const QString &key, const QString &name, const QString &searchText) {
        auto *button = card->findChild<Button *>("libraryOrganize");
        if (!button) {
            button = new Button("Organize…", card);
            button->setObjectName("libraryOrganize");
            button->setProperty("themeIcon", "settings");
            button->setIcon(Theme::coloredIcon("settings", QColor(Theme::textSecondary()), 14));
            if (auto *actions = card->findChild<QHBoxLayout *>("libraryItemActions"))
                actions->insertWidget(0, button);
            else card->layout()->addWidget(button);
            connect(button, &QPushButton::clicked, this, [this, key, name] { organize(key, name); });
        }
        const auto meta = m_memberships.value(key).toObject();
        const bool pinned = meta["pinned"].toBool();
        button->setText("Organize");
        const QString iconName = pinned ? "check" : "settings";
        button->setProperty("themeIcon", iconName);
        button->setIcon(Theme::coloredIcon(iconName, QColor(Theme::textSecondary()), 14));
        button->setToolTip(pinned ? "Pinned · Edit organization" : "Pin or assign to a project or folder");
        button->setMinimumWidth(108);
        const bool inScope = m_scope.isEmpty() || (m_folderScope ? meta["folder"].toString() == m_scope
                                                               : meta["projects"].toArray().contains(m_scope));
        const bool show = inScope && (!m_pinned->isChecked() || pinned) && searchText.contains(query, Qt::CaseInsensitive);
        card->setVisible(show);
        return show;
    };
    int visible = 0;
    QList<QWidget *> orderedVisible;
    const auto sortCards = [this](auto &cards) {
        const int sort = m_sort->currentIndex();
        std::stable_sort(cards.begin(), cards.end(), [sort](auto *a, auto *b) {
            if (sort == 1) return a->data()["name"].toString(a->id()).compare(b->data()["name"].toString(b->id()), Qt::CaseInsensitive) < 0;
            if (sort == 2) return a->data()["size_mb"].toDouble() > b->data()["size_mb"].toDouble();
            return a->data()["downloaded_at"].toString(a->data()["trained_at"].toString()) >
                   b->data()["downloaded_at"].toString(b->data()["trained_at"].toString());
        });
        if (cards.isEmpty()) return;
        auto *layout = cards.first()->parentWidget()->layout();
        for (auto *card : cards) layout->removeWidget(card);
        for (auto *card : cards) layout->addWidget(card);
    };
    if (adapters) {
        auto cards = m_adapters->findChildren<AdapterCard *>();
        sortCards(cards);
        for (auto *card : cards) {
            QString searchText = card->id();
            for (auto *label : card->findChildren<QLabel *>()) searchText += " " + label->text();
            if (apply(card, "adapter:" + card->id(), card->id(), searchText)) orderedVisible.append(card);
        }
    } else {
        auto cards = m_models->findChildren<ModelCard *>();
        sortCards(cards);
        for (auto *card : cards) {
            const bool imported = card->data()["source"].toString() == "local";
            if ((m_filter->currentIndex() == 1 && imported) || (m_filter->currentIndex() == 2 && !imported)) {
                card->hide();
                continue;
            }
            const auto name = card->data().value("ref").toString(card->id());
            if (apply(card, "model:" + card->id(), name, name + " " + card->id() + " " + card->data()["name"].toString())) orderedVisible.append(card);
        }
    }
    visible = orderedVisible.size();
    const auto rows = adapters ? m_adapters->findChildren<QFrame *>() : m_models->findChildren<QFrame *>();
    for (auto *row : rows) {
        if (!row->property("libraryRow").toBool()) continue;
        const bool last = !orderedVisible.isEmpty() && row == orderedVisible.last();
        if (row->property("lastRow").toBool() == last) continue;
        row->setProperty("lastRow", last);
        row->style()->unpolish(row);
        row->style()->polish(row);
    }
    m_results->setText(visible ? QString("%1 %2").arg(visible).arg(adapters ? (visible == 1 ? "adapter" : "adapters") : (visible == 1 ? "model" : "models")) : "No items match this view.");
}

namespace {
QString collectionKey(bool folder, const QString &name) {
    return (folder ? "folder:" : "project:") + name;
}

QVBoxLayout *listSurface(QVBoxLayout *parent) {
    auto *surface = new QFrame(parent->parentWidget());
    surface->setObjectName("libraryListSurface");
    auto *layout = new QVBoxLayout(surface);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);
    parent->addWidget(surface);
    return layout;
}

QHBoxLayout *listRow(QVBoxLayout *list, const QString &name, const QString &subtitle, const QString &iconName) {
    auto *frame = new QFrame(list->parentWidget());
    frame->setObjectName("libraryListRow");
    auto *row = new QHBoxLayout(frame);
    row->setContentsMargins(14, 11, 14, 11);
    auto *icon = new QLabel(frame);
    icon->setProperty("themeIcon", iconName);
    icon->setPixmap(Theme::coloredIcon(iconName, QColor(Theme::textSecondary()), 16).pixmap(16, 16));
    row->addWidget(icon);
    auto *identity = new QVBoxLayout;
    identity->setSpacing(3);
    auto *title = text(name, "hubSectionTitle", frame);
    title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    identity->addWidget(title);
    if (!subtitle.isEmpty()) {
        auto *label = text(subtitle, "hubMeta", frame);
        label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
        identity->addWidget(label);
    }
    row->addLayout(identity, 1);
    if (list->count()) list->itemAt(list->count() - 1)->widget()->setProperty("lastRow", false);
    frame->setProperty("lastRow", true);
    list->addWidget(frame);
    return row;
}
}

void LibraryPanel::renderCollections() {
    const bool folder = m_tabs->currentIndex() == Folders;
    auto *layout = folder ? m_folders : m_projects;
    clear(layout);
    if (!(folder ? m_openFolder : m_openProject).isEmpty()) {
        renderCollectionContents(layout, folder);
        return;
    }
    const auto &names = folder ? m_folderNames : m_projectNames;
    if (names.isEmpty()) {
        layout->addWidget(text(folder ? "No folders yet. Create one to organize your Library."
                                      : "No projects yet. Create one to bring a workflow together.",
                               "hubMeta", layout->parentWidget()));
        return;
    }
    auto *list = listSurface(layout);
    for (const auto &entry : names) {
        const auto name = entry.toString();
        const auto meta = m_collectionInfo[collectionKey(folder, name)].toObject();
        const auto belongs = [this, folder, &name](const QString &id) {
            const auto assignment = m_memberships.value(id).toObject();
            return folder ? assignment["folder"].toString() == name : assignment["projects"].toArray().contains(name);
        };
        int count = meta["files"].toArray().size();
        for (auto *item : m_models->findChildren<ModelCard *>()) count += belongs("model:" + item->id());
        for (auto *item : m_adapters->findChildren<AdapterCard *>()) count += belongs("adapter:" + item->id());
        if (m_chatStore) {
            for (const auto &item : m_chatStore->list()) count += belongs("chat:" + item.id);
        }
        const auto description = meta["description"].toString();
        const auto summary = QString(count == 1 ? "%1 item" : "%1 items").arg(count);
        auto *row = listRow(list, name, description.isEmpty() ? summary : summary + " · " + description,
                            folder ? "folder" : "layers");
        auto *remove = new Button("Delete…", row->parentWidget());
        remove->setObjectName("destructiveBtn");
        remove->setAccessibleName("Delete " + name);
        connect(remove, &QPushButton::clicked, this, [this, folder, name] { deleteCollection(folder, name); });
        row->addWidget(remove);
        auto *open = new Button("Open", row->parentWidget());
        open->setAccessibleName("Open " + name);
        open->setIcon(Theme::coloredIcon("chevron-right", QColor(Theme::textSecondary()), 14));
        open->setProperty("themeIcon", "chevron-right");
        open->setMinimumWidth(84);
        row->addWidget(open);
        connect(open, &QPushButton::clicked, this, [this, folder, name] {
            (folder ? m_openFolder : m_openProject) = name;
            QTimer::singleShot(0, this, &LibraryPanel::updateView);
        });
    }
}

void LibraryPanel::createCollection() {
    const bool folder = m_tabs->currentIndex() == Folders;
    auto &names = folder ? m_folderNames : m_projectNames;
    Ui::LibraryDialog dialog(this, folder ? "New folder" : "New project", folder ? "Create folder" : "Create project");
    auto *name = new QLineEdit(&dialog);
    name->setMaxLength(80);
    name->setPlaceholderText(folder ? "e.g. Reference material" : "e.g. Local support assistant");
    Surface::field(dialog.form(), folder ? "What should this folder be called?" : "What should this project be called?", name);
    auto *description = new QPlainTextEdit(&dialog);
    description->setPlaceholderText(folder ? "Describe what belongs here" : "Describe what you want to accomplish");
    description->setFixedHeight(76);
    Surface::field(dialog.form(), folder ? "What will you keep here? (optional)" : "What are you working on? (optional)", description);
    auto *instructions = new QPlainTextEdit(&dialog);
    if (!folder) {
        instructions->setPlaceholderText("Add notes, requirements, or instructions for people using this project");
        instructions->setFixedHeight(86);
        Surface::field(dialog.form(), "What should you keep in mind? (optional)", instructions);
    } else instructions->hide();
    auto *error = text({}, "hubSmallMeta", &dialog);
    dialog.form()->addWidget(error);
    auto validate = [&] {
        const auto title = name->text().trimmed();
        bool duplicate = false;
        for (const auto &entry : names) duplicate |= entry.toString().compare(title, Qt::CaseInsensitive) == 0;
        error->setText(duplicate ? "That name is already in use." : QString());
        error->setVisible(duplicate);
        dialog.acceptButton()->setEnabled(!title.isEmpty() && !duplicate);
    };
    connect(name, &QLineEdit::textChanged, &dialog, validate);
    validate();
    dialog.addActions();
    name->setFocus();
    if (dialog.exec() != QDialog::Accepted) return;
    const auto title = name->text().trimmed();
    names.append(title);
    m_collectionInfo[collectionKey(folder, title)] = QJsonObject{
        {"description", description->toPlainText().trimmed()}, {"instructions", instructions->toPlainText().trimmed()}};
    (folder ? m_openFolder : m_openProject) = title;
    save();
    updateView();
}

void LibraryPanel::deleteCollection(bool folder, const QString &name) {
    Ui::LibraryDialog dialog(this, folder ? "Delete folder" : "Delete project",
                             folder ? "Delete folder" : "Delete project");
    dialog.acceptButton()->setProperty("primary", false);
    dialog.acceptButton()->setObjectName("destructiveBtn");
    dialog.form()->addWidget(text(QString("Delete “%1” from your Library?").arg(name), "hubBody", &dialog));
    dialog.form()->addWidget(text("Your chats, files, models, and adapters will stay where they are.",
                                  "hubMeta", &dialog));
    dialog.addActions();
    if (dialog.exec() != QDialog::Accepted) return;

    auto &names = folder ? m_folderNames : m_projectNames;
    for (qsizetype i = 0; i < names.size(); ++i) {
        if (names.at(i).toString() != name) continue;
        names.removeAt(i);
        break;
    }
    m_collectionInfo.remove(collectionKey(folder, name));
    for (auto it = m_memberships.begin(); it != m_memberships.end(); ++it) {
        auto assignment = it.value().toObject();
        if (folder) {
            if (assignment["folder"].toString() != name) continue;
            assignment.remove("folder");
        } else {
            auto projects = assignment["projects"].toArray();
            if (!projects.contains(name)) continue;
            for (qsizetype i = projects.size(); i > 0; --i) {
                if (projects.at(i - 1).toString() == name) projects.removeAt(i - 1);
            }
            assignment["projects"] = projects;
        }
        it.value() = assignment;
    }
    auto &open = folder ? m_openFolder : m_openProject;
    if (open == name) open.clear();
    save();
    QTimer::singleShot(0, this, &LibraryPanel::updateView);
}

void LibraryPanel::renderCollectionContents(QVBoxLayout *layout, bool folder) {
    const auto name = folder ? m_openFolder : m_openProject;
    const auto key = collectionKey(folder, name);
    const auto meta = m_collectionInfo[key].toObject();
    auto *back = new Button(folder ? "← All folders" : "← All projects", layout->parentWidget());
    back->setFlat(true);
    layout->addWidget(back, 0, Qt::AlignLeft);
    connect(back, &QPushButton::clicked, this, [this, folder] {
        (folder ? m_openFolder : m_openProject).clear();
        QTimer::singleShot(0, this, &LibraryPanel::updateView);
    });
    auto *header = Surface::card(layout->parentWidget());
    header->addWidget(text(name, "hubPageTitle", header->parentWidget()));
    for (const auto &field : {"description", "instructions"}) {
        const auto value = meta[field].toString();
        if (!value.isEmpty()) {
            if (QString::fromLatin1(field) == "instructions") header->addWidget(text("Project notes", "hubSectionTitle", header->parentWidget()));
            header->addWidget(text(value, "hubBody", header->parentWidget()));
        }
    }
    auto *actions = new QHBoxLayout;
    auto *chat = new Button("Move chat here…", header->parentWidget());
    chat->setEnabled(m_chatStore && !m_chatStore->list().isEmpty());
    if (!chat->isEnabled()) chat->setToolTip("Save a chat in Conversation first.");
    connect(chat, &QPushButton::clicked, this, [this, folder] { addChat(folder); });
    actions->addWidget(chat);
    actions->addStretch();
    auto *remove = new Button(folder ? "Delete folder…" : "Delete project…", header->parentWidget());
    remove->setObjectName("destructiveBtn");
    connect(remove, &QPushButton::clicked, this, [this, folder, name] { deleteCollection(folder, name); });
    actions->addWidget(remove);
    header->addLayout(actions);
    layout->addWidget(header->parentWidget());
    layout->addWidget(text("Contents", "hubSectionTitle", layout->parentWidget()));
    auto *list = listSurface(layout);
    const auto belongs = [this, folder, &name](const QString &id) {
        const auto assignment = m_memberships[id].toObject();
        return folder ? assignment["folder"].toString() == name : assignment["projects"].toArray().contains(name);
    };
    if (m_chatStore) {
        for (const auto &item : m_chatStore->list()) {
            if (!belongs("chat:" + item.id)) continue;
            auto *row = listRow(list, item.title, "Chat", "message-circle");
            auto *open = new Button("Open chat", row->parentWidget());
            connect(open, &QPushButton::clicked, this, [this, id = item.id] { emit openChatRequested(id); });
            row->addWidget(open);
        }
    }
    for (const auto &entry : meta["files"].toArray()) {
        const auto path = entry.toString();
        const QFileInfo info(path);
        const bool exists = info.isFile();
        auto *row = listRow(list, info.fileName(), exists ? "Linked file" : "File unavailable", "file-text");
        auto *open = new Button("Open file", row->parentWidget());
        open->setEnabled(exists);
        row->addWidget(open);
        connect(open, &QPushButton::clicked, this, [this, path] {
            QDesktopServices::openUrl(QUrl::fromLocalFile(path));
        });
        auto *unlink = new Button("Remove link", row->parentWidget());
        row->addWidget(unlink);
        connect(unlink, &QPushButton::clicked, this, [this, key, path] {
            auto updated = m_collectionInfo[key].toObject();
            QJsonArray files;
            for (const auto &entry : updated["files"].toArray()) if (entry.toString() != path) files.append(entry);
            updated["files"] = files;
            m_collectionInfo[key] = updated;
            save();
            QTimer::singleShot(0, this, &LibraryPanel::renderCollections);
        });
    }
    for (auto *item : m_models->findChildren<ModelCard *>()) {
        if (!belongs("model:" + item->id())) continue;
        const auto modelName = item->data()["name"].toString(item->id());
        auto *row = listRow(list, modelName, "Model · " + ModelCard::formatSizeMb(item->data()["size_mb"].toDouble()), "box");
        auto *load = new Button("Load model", row->parentWidget());
        connect(load, &QPushButton::clicked, this, [this, id = item->localId()] { emit m_models->loadRequested(id); });
        row->addWidget(load);
    }
    for (auto *item : m_adapters->findChildren<AdapterCard *>()) {
        if (!belongs("adapter:" + item->id())) continue;
        auto *row = listRow(list, item->data()["name"].toString(item->id()),
                            "Adapter · " + item->data()["base_model"].toString(), "layers");
        auto *activate = new Button(item->property("active").toBool() ? "Deactivate" : "Activate", row->parentWidget());
        connect(activate, &QPushButton::clicked, this, [this, id = item->id()] {
            for (auto *card : m_adapters->findChildren<AdapterCard *>()) {
                if (card->id() == id) {
                    emit card->activateRequested(card);
                    break;
                }
            }
        });
        row->addWidget(activate);
    }
    if (!list->count()) listRow(list, "Nothing here yet", "Move a chat here, add a file, or use Organize on a model or adapter.", "folder");
}

void LibraryPanel::addChat(bool folder) {
    if (!m_chatStore) return;
    Ui::LibraryDialog dialog(this, "Move chat", "Move chat");
    dialog.form()->addWidget(text("Move a saved chat into “" + (folder ? m_openFolder : m_openProject) + "”.",
                                  "hubBody", &dialog));
    auto *choices = new ComboBox(&dialog);
    choices->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    choices->setMinimumContentsLength(24);
    for (const auto &item : m_chatStore->list()) choices->addItem(item.title, item.id);
    Surface::field(dialog.form(), "Which chat should go here?", choices);
    dialog.acceptButton()->setEnabled(choices->count() > 0);
    dialog.addActions();
    if (dialog.exec() != QDialog::Accepted) return;
    const auto key = "chat:" + choices->currentData().toString();
    auto meta = m_memberships[key].toObject();
    if (folder) meta["folder"] = m_openFolder;
    else meta["projects"] = QJsonArray{m_openProject};
    m_memberships[key] = meta;
    save();
    QTimer::singleShot(0, this, &LibraryPanel::renderCollections);
}

void LibraryPanel::addFiles(bool folder) {
    const auto paths = QFileDialog::getOpenFileNames(this, "Link files to " + (folder ? m_openFolder : m_openProject),
                                                   {}, "Documents (*.md *.txt *.pdf *.json *.csv);;All files (*)");
    if (paths.isEmpty()) return;
    const auto key = collectionKey(folder, folder ? m_openFolder : m_openProject);
    auto meta = m_collectionInfo[key].toObject();
    auto files = meta["files"].toArray();
    for (const auto &path : paths) if (!files.contains(path)) files.append(path);
    meta["files"] = files;
    m_collectionInfo[key] = meta;
    save();
    renderCollections();
}

void LibraryPanel::organize(const QString &key, const QString &name) {
    const auto meta = m_memberships.value(key).toObject();
    Ui::LibraryDialog dialog(this, "Organize item", "Save changes");
    auto *layout = dialog.form();
    layout->addWidget(text(name, "hubSectionTitle", &dialog));
    auto *folder = new ComboBox(&dialog);
    folder->addItem("No folder", QString());
    for (const auto &entry : m_folderNames) folder->addItem(entry.toString(), entry.toString());
    folder->setCurrentIndex(qMax(0, folder->findData(meta["folder"].toString())));
    Surface::field(layout, "Folder", folder);
    layout->addWidget(text("Projects", "hubMeta", &dialog));
    auto *scroll = new QScrollArea(&dialog);
    scroll->setWidgetResizable(true);
    auto *body = new QWidget;
    auto *projectLayout = new QVBoxLayout(body);
    QList<QCheckBox *> projects;
    for (const auto &entry : m_projectNames) {
        auto *check = new QCheckBox(entry.toString(), body);
        check->setChecked(meta["projects"].toArray().contains(entry));
        projectLayout->addWidget(check);
        projects.append(check);
    }
    if (projects.isEmpty()) projectLayout->addWidget(text("Create a project in the Projects tab.", "hubMeta", body));
    projectLayout->addStretch();
    scroll->setWidget(body);
    layout->addWidget(scroll);
    auto *pin = new QCheckBox("Pin item", &dialog);
    pin->setChecked(meta["pinned"].toBool());
    layout->addWidget(pin);
    dialog.addActions();
    if (dialog.exec() != QDialog::Accepted) return;
    QJsonArray selected;
    for (int i = 0; i < projects.size(); ++i) if (projects[i]->isChecked()) selected.append(m_projectNames[i]);
    m_memberships[key] = QJsonObject{{"folder", folder->currentData().toString()},
                                    {"projects", selected}, {"pinned", pin->isChecked()}};
    save();
    filterItems();
}

void LibraryPanel::recordActivity(const QString &name, const QString &action, const QString &status,
                                  const QString &detail) {
    m_events.prepend(QJsonObject{{"name", name}, {"action", action}, {"status", status},
                                 {"detail", detail}, {"time", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)}});
    while (m_events.size() > 500) m_events.removeLast();
    save();
    refreshStorage();
    if (m_tabs->currentIndex() == History) renderHistory();
}

void LibraryPanel::renderHistory() {
    clear(m_history);
    const auto query = m_search->text().trimmed();
    QVBoxLayout *list = nullptr;
    for (int i = 0; i < m_events.size(); ++i) {
        const auto value = m_events.at(m_sort->currentIndex() == 1 ? m_events.size() - 1 - i : i);
        const auto entry = value.toObject();
        const auto name = entry["name"].toString();
        const auto action = entry["action"].toString();
        const auto status = entry["status"].toString();
        if (m_filter->currentIndex() > 0 && status != m_filter->currentText()) continue;
        if (!(name + " " + action + " " + status).contains(query, Qt::CaseInsensitive)) continue;
        if (!list) list = listSurface(m_history);
        const auto detail = entry["detail"].toString();
        auto *row = listRow(list, name, detail, action == "Download" ? "download" : "file-text");
        auto *statusLabel = text(action + " · " + status, "hubMeta", row->parentWidget());
        statusLabel->setMinimumWidth(135);
        row->addWidget(statusLabel);
        const auto date = QDateTime::fromString(entry["time"].toString(), Qt::ISODate).toLocalTime();
        row->addWidget(text(date.toString("MMM d, yyyy · HH:mm"), "hubSmallMeta", row->parentWidget()));
        auto *retry = new Button("Retry…", row->parentWidget());
        retry->setFixedWidth(78);
        if (action == "Download" && status != "Completed") {
            connect(retry, &QPushButton::clicked, this, [this, name] { emit retryDownload(name); });
        } else {
            retry->setText({});
            retry->setFlat(true);
            retry->setDisabled(true);
            retry->setFocusPolicy(Qt::NoFocus);
        }
        row->addWidget(retry);
    }
    m_results->setText(QString("%1 activity entries").arg(list ? list->count() : 0));
    if (!list) m_history->addWidget(text(query.isEmpty() ? "No library activity yet." : "No matching activity.", "hubMeta", m_history->parentWidget()));
}

QStringList LibraryPanel::chatProjects(const QString &id) const {
    QStringList projects;
    const auto assigned = m_memberships.value("chat:" + id).toObject().value("projects").toArray();
    for (const auto &name : m_projectNames) {
        if (assigned.contains(name)) projects.append(name.toString());
    }
    return projects;
}

void LibraryPanel::save() {
    QSettings().setValue("library/organization", QJsonDocument(QJsonObject{{"projects", m_projectNames},
                         {"folders", m_folderNames}, {"items", m_memberships}, {"history", m_events}, {"collections", m_collectionInfo}}).toJson(QJsonDocument::Compact));
    emit organizationChanged();
}
