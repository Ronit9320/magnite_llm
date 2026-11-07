#pragma once

#include "panels/chatstore.h"

#include <QList>
#include <QString>
#include <QWidget>

class QListWidget;
class QMenu;
class QPushButton;

// The saved chats under "Playground" in the left menu: a "New chat" row and a list that shows about
// five chats and scrolls for the rest. It only asks (signals); the controller does the work.
class ChatHistoryList : public QWidget {
    Q_OBJECT

public:
    static constexpr int kVisibleRows = 5;
    static constexpr int kRowHeight = 30;

    explicit ChatHistoryList(QWidget *parent = nullptr);

    // Replaces the rows. The open chat is highlighted; the scroll position is kept.
    void setChats(const QList<ChatSummary> &chats, const QString &openId);
    // While busy (a reply streaming, a send prepared, a model loading, an editor open): New chat and opening
    // are off, and so are the open chat's own actions.
    void setBusy(bool busy);
    // Re-tints icons for the current light or dark theme.
    void refreshIcons();
    // The Rename / Delete menu for one chat. The caller shows it (exec) and deletes it.
    QMenu *contextMenuFor(const QString &id);

signals:
    void newChatRequested();
    void openRequested(const QString &id);
    void renameRequested(const QString &id);
    void deleteRequested(const QString &id);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    QString idOf(int row) const;
    void highlightOpen();
    void requestOpen(const QString &id);
    void requestDelete(const QString &id);
    void showMenuAt(const QPoint &viewportPos);
    void updateHeights();

    QPushButton *m_new = nullptr;
    QListWidget *m_list = nullptr;
    QString m_openId;
    bool m_busy = false;
};
