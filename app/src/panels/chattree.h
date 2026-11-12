#pragma once

#include <QJsonObject>
#include <QJsonArray>
#include <QList>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

// The conversation as a tree of messages. Siblings are the versions of a message
// (edited user messages, retried replies); exactly one child per node is "active",
// and following the active children from the root gives the path shown on screen.
// Pure data, no widgets, so the UI check can exercise it directly.
class ChatTree {
public:
    using NodeId = int;                    // 0 is the virtual root; real ids start at 1 and are not reused until clear()
    static constexpr NodeId kRoot = 0;
    // `covered` leading messages of the active path are replaced by `summary` when sending.
    struct Compaction {
        QString summary;
        int covered = 0;
    };

    // Adds the newest version under `parent` and makes it the parent's active child.
    NodeId addChild(NodeId parent, const QString &role, const QString &content);
    NodeId parentOf(NodeId id) const;
    QString role(NodeId id) const;
    QString content(NodeId id) const;

    // Root excluded, oldest first.
    QList<NodeId> activePath() const;
    int pathIndexOf(NodeId id) const;      // -1 when not on the active path
    NodeId lastOnPath() const;             // kRoot when empty

    QList<NodeId> versionsOf(NodeId id) const;   // siblings including `id`, in creation order
    int versionIndex(NodeId id) const;           // 0-based within versionsOf
    int versionCount(NodeId id) const;
    // Makes `id` its parent's active child. Its own remembered continuation comes back with it.
    void activate(NodeId id);

    // {role, content} of the first `pathCount` messages on the active path (-1 = all).
    QList<QJsonObject> messages(int pathCount = -1) const;

    // Things the transcript shows before a message that are not messages (compaction dividers).
    void addNote(NodeId id, const QString &text);
    QStringList notes(NodeId id) const;

    // A compaction made while sending `id`. The one in effect is the last snapshot among the
    // first `pathCount` path nodes (-1 = all); covered == 0 means none.
    void setCompaction(NodeId id, const Compaction &compaction);
    Compaction compaction(int pathCount = -1) const;

    QJsonObject knowledge() const { return m_knowledge; }
    void setKnowledge(const QJsonObject &knowledge) { m_knowledge = knowledge; }
    QJsonArray citations(NodeId id) const { return valid(id) ? node(id).citations : QJsonArray(); }
    void setCitations(NodeId id, const QJsonArray &citations) { if (valid(id)) node(id).citations = citations; }

    bool isEmpty() const;
    void clear();

    // The whole tree as plain JSON, every version included, ids preserved:
    // {"nodes":[{id,parent,role,content,children,activeChild,notes,compaction}],"rootChildren":[…],"rootActive":id}.
    QJsonObject toJson() const;
    // nullopt unless the JSON is a complete, self-consistent tree (ids, parent/child links, reachability,
    // active children, roles, snapshots). Nothing half-valid is ever returned.
    static std::optional<ChatTree> fromJson(const QJsonObject &json);

private:
    struct Node {
        NodeId parent = kRoot;
        QString role;
        QString content;
        QList<NodeId> children;
        NodeId activeChild = kRoot;
        QStringList notes;
        Compaction compaction;
        QJsonArray citations;
    };
    bool valid(NodeId id) const { return id >= 1 && id <= m_nodes.size(); }
    Node &node(NodeId id) { return m_nodes[id - 1]; }
    const Node &node(NodeId id) const { return m_nodes.at(id - 1); }

    QJsonObject m_knowledge;
    QVector<Node> m_nodes;                 // node `id` lives at index id - 1
    QList<NodeId> m_rootChildren;
    NodeId m_rootActive = kRoot;
};
