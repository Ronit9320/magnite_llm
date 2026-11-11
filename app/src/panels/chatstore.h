#pragma once

#include "panels/chattree.h"

#include <QDateTime>
#include <QHash>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

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
    // <data folder>/chats, the data folder being MAGNITE_HOME, else the Settings data directory, else ~/.magnite (%APPDATA%/Magnite on Windows).
    static QString defaultDir();

    // Newest activity first. Built once from the folder, then kept up to date in memory.
    QList<ChatSummary> list() const { return m_chats; }
    QString create();                                          // a fresh valid id; writes nothing
    bool save(const QString &id, const ChatTree &tree);        // atomic; false + lastError() on failure
    std::optional<ChatTree> load(const QString &id) const;     // nullopt for anything missing, unreadable, too new or inconsistent
    bool rename(const QString &id, const QString &title);
    bool remove(const QString &id);
    QString directory() const { return m_dir; }
    bool exportAll(QJsonObject *result, QString *error) const;
    QString lastError() const { return m_lastError; }
    // Chat files that exist but could not be read (damaged, from a newer version, inconsistent). They are never
    // modified or deleted; the sidebar tells the user how many there are. Sorted.
    QStringList unreadableIds() const { return m_unreadable; }

    static QString titleFor(const ChatTree &tree);
    static bool validId(const QString &id);

signals:
    void changed();   // after every successful save, rename or remove

private:
    struct Parsed {
        ChatSummary summary;
        ChatTree tree;
    };
    // What the launch index remembers about one chat file; trusted only while the file's size and time still match.
    struct IndexEntry {
        qint64 size = 0;
        qint64 mtime = 0;
        bool readable = false;
        ChatSummary summary;
    };
    QString indexPath() const { return m_dir + ".index"; }   // beside the folder: the chats folder holds chats only
    QHash<QString, IndexEntry> loadIndex() const;
    void saveIndex(const QHash<QString, IndexEntry> &entries) const;
    QString pathFor(const QString &id) const;
    std::optional<Parsed> read(const QString &id) const;
    bool write(const QJsonObject &file, const QString &id);
    void upsert(const ChatSummary &summary);
    QDateTime nextStamp() const;

    QString m_dir;
    QList<ChatSummary> m_chats;
    QStringList m_unreadable;
    QString m_lastError;
};
