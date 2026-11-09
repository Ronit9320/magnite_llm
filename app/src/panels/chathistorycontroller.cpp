#include "panels/chathistorycontroller.h"

#include "panels/chatpanel.h"
#include "panels/chatstore.h"
#include "widgets/chathistorylist.h"
#include "widgets/confirmation.h"

#include <QInputDialog>
#include <QLineEdit>
#include <QMetaObject>

ChatHistoryController::ChatHistoryController(ChatStore *store, ChatPanel *panel, ChatHistoryList *list, QWidget *dialogParent, QObject *parent)
    : QObject(parent)
    , m_store(store)
    , m_panel(panel)
    , m_list(list) {
    askTitle = [dialogParent](const QString &current) {
        bool ok = false;
        const QString title = QInputDialog::getText(dialogParent, "Rename chat", "Chat name", QLineEdit::Normal, current, &ok);
        return ok ? title : QString();
    };
    confirmDelete = [dialogParent](const QString &title) {
        return Ui::confirmRemoval(dialogParent, "Delete chat?",
                                  "“" + title + "” and all its versions will be removed. This cannot be undone.", "Delete");
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
}
