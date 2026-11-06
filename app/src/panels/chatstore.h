#pragma once

#include "panels/chattree.h"

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>

#include <optional>

// One row of the saved-chats list.
struct ChatSummary {
    QString id;
    QString title;
    bool customTitle = false;   // true once the user renamed it; the title then stops following the first message
    QDateTime created;
    QDateTime updated;
};

class ChatStore : public QObject {
    Q_OBJECT

public:
    explicit ChatStore(const QString &chatsDir, QObject *parent = nullptr);
    // <data folder>/chats, the data folder being MAGNITE_HOME, else the Settings data directory, else ~/.magnite.
    static QString defaultDir();

    // Newest activity first. Built once from the folder, then kept up to date in memory.
    QList<ChatSummary> list() const { return m_chats; }
    QString create();                                          // a fresh valid id; writes nothing
    bool save(const QString &id, const ChatTree &tree);        // atomic; false + lastError() on failure
    std::optional<ChatTree> load(const QString &id) const;     // nullopt for anything missing, unreadable, too new or inconsistent
    bool rename(const QString &id, const QString &title);
    bool remove(const QString &id);
    QString lastError() const { return m_lastError; }

    static QString titleFor(const ChatTree &tree);
    static bool validId(const QString &id);

signals:
    void changed();   // after every successful save, rename or remove

private:
    struct Parsed {
        ChatSummary summary;
        ChatTree tree;
    };
    QString pathFor(const QString &id) const;
    std::optional<Parsed> read(const QString &id) const;
    bool write(const QJsonObject &file, const QString &id);
    void upsert(const ChatSummary &summary);
    QDateTime nextStamp() const;

    QString m_dir;
    QList<ChatSummary> m_chats;
    QString m_lastError;
};
