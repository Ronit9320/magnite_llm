#include "panels/chathistorycontroller.h"

#include "panels/chatpanel.h"
#include "panels/chatstore.h"
#include "widgets/chathistorylist.h"
#include "widgets/librarydialog.h"
#include <QLineEdit>
#include <QMetaObject>

ChatHistoryController::ChatHistoryController(ChatStore *store, ChatPanel *panel, ChatHistoryList *list, QWidget *dialogParent, QObject *parent)
    : QObject(parent)
    , m_store(store)
    , m_panel(panel)
    , m_list(list) {
    askTitle = [dialogParent](const QString &current) {
        Ui::LibraryDialog dialog(dialogParent, "Rename conversation", "Save");
        auto *name = new QLineEdit(current, &dialog);
        auto *label = new QLabel("Conversation name", &dialog);
        label->setObjectName("hubMeta");
        label->setBuddy(name);
        name->setAccessibleName("Conversation name");
        dialog.form()->addWidget(label);
        dialog.form()->addWidget(name);
        dialog.addActions();
        const auto validate = [name, &dialog] {
            dialog.acceptButton()->setEnabled(!name->text().trimmed().isEmpty());
        };
        QObject::connect(name, &QLineEdit::textChanged, &dialog, validate);
        QObject::connect(name, &QLineEdit::returnPressed, dialog.acceptButton(), &QPushButton::click);
        validate();
        name->setFocus();
        name->selectAll();
        return dialog.exec() == QDialog::Accepted ? name->text().trimmed() : QString();
    };
    confirmDelete = [dialogParent](const QString &title) {
        Ui::LibraryDialog dialog(dialogParent, "Delete conversation", "Delete");
        dialog.acceptButton()->setProperty("primary", false);
        dialog.acceptButton()->setObjectName("destructiveBtn");
        auto *description = new QLabel(QString("Delete “%1” and all its versions?").arg(title), &dialog);
        description->setObjectName("hubBody");
        description->setTextFormat(Qt::PlainText);
        description->setWordWrap(true);
        dialog.form()->addWidget(description);
        dialog.addActions();
        return dialog.exec() == QDialog::Accepted;
    };

    connect(store, &ChatStore::changed, this, &ChatHistoryController::refresh);
    connect(panel, &ChatPanel::currentChatChanged, this, &ChatHistoryController::refresh);
    connect(panel, &ChatPanel::busyChanged, list, &ChatHistoryList::setBusy);
    connect(list, &ChatHistoryList::openRequested, this, [this](const QString &id) { m_panel->openChat(id); });
    connect(list, &ChatHistoryList::newChatRequested, this, [this]() { QMetaObject::invokeMethod(m_panel, "onClearChat"); });
    connect(list, &ChatHistoryList::renameRequested, this, [this](const QString &id) {
        const QString title = askTitle(titleOf(id));
        if (!title.trimmed().isEmpty()) m_panel->renameChat(id, title);
    });
    connect(list, &ChatHistoryList::deleteRequested, this, [this](const QString &id) {
        if (confirmDelete(titleOf(id))) m_panel->deleteChat(id);
    });
    list->setBusy(panel->isBusy());
    refresh();
}

QString ChatHistoryController::titleOf(const QString &id) const {
    for (const ChatSummary &row : m_store->list())
        if (row.id == id) return row.title;
    return QString();
}

void ChatHistoryController::refresh() {
    m_list->setChats(m_store->list(), m_panel->currentChatId());
    m_list->setUnreadableCount(m_store->unreadableIds().size());
}
