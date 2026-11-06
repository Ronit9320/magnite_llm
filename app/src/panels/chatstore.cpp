#include "panels/chatstore.h"

#include <algorithm>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSaveFile>
#include <QSettings>
#include <QUuid>

namespace {
constexpr int kFileVersion = 1;
constexpr int kTitleLength = 40;

QString stamp(const QDateTime &time) { return time.toUTC().toString(Qt::ISODateWithMs); }
QDateTime parseStamp(const QJsonValue &value) {
    return value.isString() ? QDateTime::fromString(value.toString(), Qt::ISODateWithMs) : QDateTime();
}
}  // namespace

ChatStore::ChatStore(const QString &chatsDir, QObject *parent)
    : QObject(parent)
    , m_dir(QDir::cleanPath(chatsDir)) {
    // One read at launch for titles and dates. Unreadable or inconsistent files are skipped and left alone.
    const QStringList names = QDir(m_dir).entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString &name : names) {
        const QString id = name.chopped(5);
        if (!validId(id)) continue;
        if (const auto parsed = read(id)) m_chats.append(parsed->summary);
    }
    std::sort(m_chats.begin(), m_chats.end(), [](const ChatSummary &a, const ChatSummary &b) {
        return a.updated != b.updated ? a.updated > b.updated : a.id < b.id;
    });
}

QString ChatStore::defaultDir() {
    QString home = qEnvironmentVariable("MAGNITE_HOME");
    if (home.isEmpty()) home = QSettings().value("storage/dataDir").toString().trimmed();
    if (home.isEmpty()) home = QDir::homePath() + "/.magnite";
    return QFileInfo(home).absoluteFilePath() + "/chats";
}

bool ChatStore::validId(const QString &id) {
    static const QRegularExpression pattern(QStringLiteral("^[0-9a-f]{8}(-[0-9a-f]{4}){3}-[0-9a-f]{12}$"));
    return pattern.match(id).hasMatch();
}

QString ChatStore::pathFor(const QString &id) const { return m_dir + "/" + id + ".json"; }

QString ChatStore::create() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

QString ChatStore::titleFor(const ChatTree &tree) {
    const QList<ChatTree::NodeId> path = tree.activePath();
    if (path.isEmpty()) return QStringLiteral("New chat");
    // The first version of the first message, so editing it later does not rename the chat.
    QString text = tree.content(tree.versionsOf(path.first()).first()).simplified();
    if (text.isEmpty()) return QStringLiteral("New chat");
    if (text.size() > kTitleLength) {
        text = text.left(kTitleLength);
        if (text.at(text.size() - 1).isHighSurrogate()) text.chop(1);
        text += QChar(0x2026);
    }
    return text;
}

std::optional<ChatStore::Parsed> ChatStore::read(const QString &id) const {
    if (!validId(id)) return std::nullopt;
    QFile file(pathFor(id));
    if (!file.open(QIODevice::ReadOnly)) return std::nullopt;
    QJsonParseError error;
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError || !doc.isObject()) return std::nullopt;
    const QJsonObject o = doc.object();
    if (!o.value("version").isDouble() || o.value("version").toInt() != kFileVersion) return std::nullopt;
    if (o.value("id").toString() != id || !o.value("title").isString() || !o.value("customTitle").isBool()) return std::nullopt;
    const QDateTime created = parseStamp(o.value("created")), updated = parseStamp(o.value("updated"));
    if (!created.isValid() || !updated.isValid() || !o.value("tree").isObject()) return std::nullopt;
    auto tree = ChatTree::fromJson(o.value("tree").toObject());
    if (!tree) return std::nullopt;
    Parsed parsed;
    parsed.summary = ChatSummary{id, o.value("title").toString(), o.value("customTitle").toBool(), created, updated};
    parsed.tree = std::move(*tree);
    return parsed;
}

std::optional<ChatTree> ChatStore::load(const QString &id) const {
    if (auto parsed = read(id)) return std::move(parsed->tree);
    return std::nullopt;
}

QDateTime ChatStore::nextStamp() const {
    QDateTime now = QDateTime::currentDateTimeUtc();
    // Strictly increasing, so "newest first" is never a tie.
    if (!m_chats.isEmpty() && now <= m_chats.first().updated) now = m_chats.first().updated.addMSecs(1);
    return now;
}

bool ChatStore::write(const QJsonObject &file, const QString &id) {
    if (!QDir().mkpath(m_dir)) {
        m_lastError = "Could not create the chats folder " + m_dir;
        return false;
    }
    QSaveFile out(pathFor(id));   // writes a temporary file and renames it over the target on commit
    if (!out.open(QIODevice::WriteOnly)) {
        m_lastError = out.errorString();
        return false;
    }
    out.write(QJsonDocument(file).toJson(QJsonDocument::Indented));
    if (!out.commit()) {
        m_lastError = out.errorString();
        return false;
    }
    return true;
}

void ChatStore::upsert(const ChatSummary &summary) {
    bool found = false;
    for (ChatSummary &row : m_chats)
        if (row.id == summary.id) { row = summary; found = true; }
    if (!found) m_chats.append(summary);
    std::sort(m_chats.begin(), m_chats.end(), [](const ChatSummary &a, const ChatSummary &b) {
        return a.updated != b.updated ? a.updated > b.updated : a.id < b.id;
    });
}

bool ChatStore::save(const QString &id, const ChatTree &tree) {
    if (!validId(id)) {
        m_lastError = "Invalid chat id";
        return false;
    }
    ChatSummary summary;
    for (const ChatSummary &row : m_chats)
        if (row.id == id) summary = row;
    summary.id = id;
    if (!summary.customTitle) summary.title = titleFor(tree);
    summary.updated = nextStamp();
    if (!summary.created.isValid()) summary.created = summary.updated;
    const QJsonObject file{{"version", kFileVersion}, {"id", id}, {"title", summary.title}, {"customTitle", summary.customTitle},
                           {"created", stamp(summary.created)}, {"updated", stamp(summary.updated)}, {"tree", tree.toJson()}};
    if (!write(file, id)) return false;
    upsert(summary);
    emit changed();
    return true;
}

bool ChatStore::rename(const QString &id, const QString &title) {
    m_lastError.clear();
    const QString clean = title.trimmed();
    if (!validId(id) || clean.isEmpty()) return false;
    auto parsed = read(id);
    if (!parsed) return false;
    parsed->summary.title = clean;
    parsed->summary.customTitle = true;
    const ChatSummary &s = parsed->summary;   // renaming is not conversation activity: the order stays as it is
    const QJsonObject file{{"version", kFileVersion}, {"id", id}, {"title", s.title}, {"customTitle", true},
                           {"created", stamp(s.created)}, {"updated", stamp(s.updated)}, {"tree", parsed->tree.toJson()}};
    if (!write(file, id)) return false;
    upsert(s);
    emit changed();
    return true;
}

bool ChatStore::remove(const QString &id) {
    m_lastError.clear();
    if (!validId(id)) return false;
    const auto row = std::find_if(m_chats.begin(), m_chats.end(), [&id](const ChatSummary &s) { return s.id == id; });
    if (row == m_chats.end()) return false;
    if (!QFile::remove(pathFor(id)) && QFile::exists(pathFor(id))) {
        m_lastError = "Could not delete " + pathFor(id);
        return false;
    }
    m_chats.erase(row);
    emit changed();
    return true;
}
