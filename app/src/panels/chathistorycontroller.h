#pragma once

#include <QObject>
#include <QString>

#include <functional>

class ChatHistoryList;
class ChatPanel;
class ChatStore;
class QWidget;

// Joins the saved chats (store), the Playground (panel) and the list in the left menu, and owns the
// rename and delete questions. The questions are replaceable so checks never open a window.
class ChatHistoryController : public QObject {
    Q_OBJECT

public:
    ChatHistoryController(ChatStore *store, ChatPanel *panel, ChatHistoryList *list, QWidget *dialogParent, QObject *parent = nullptr);

    // Returns the new title, or an empty string to cancel. Default: a themed input dialog with the current title.
    std::function<QString(const QString &currentTitle)> askTitle;
    // Default: "Delete chat?" with a destructive Delete button; Cancel is the default.
    std::function<bool(const QString &title)> confirmDelete;

    void refresh();

private:
    QString titleOf(const QString &id) const;

    ChatStore *m_store;
    ChatPanel *m_panel;
    ChatHistoryList *m_list;
};
