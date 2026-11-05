#include "panels/chattree.h"

#include <algorithm>

ChatTree::NodeId ChatTree::addChild(NodeId parent, const QString &role, const QString &content) {
    Node created;
    created.parent = valid(parent) ? parent : kRoot;
    created.role = role;
    created.content = content;
    m_nodes.append(created);
    const NodeId id = m_nodes.size();
    if (created.parent == kRoot) {
        m_rootChildren.append(id);
        m_rootActive = id;
    } else {
        node(created.parent).children.append(id);
        node(created.parent).activeChild = id;
    }
    return id;
}

ChatTree::NodeId ChatTree::parentOf(NodeId id) const { return valid(id) ? node(id).parent : kRoot; }
QString ChatTree::role(NodeId id) const { return valid(id) ? node(id).role : QString(); }
QString ChatTree::content(NodeId id) const { return valid(id) ? node(id).content : QString(); }

QList<ChatTree::NodeId> ChatTree::activePath() const {
    QList<NodeId> path;
    for (NodeId id = m_rootActive; id != kRoot; id = node(id).activeChild) path.append(id);
    return path;
}

int ChatTree::pathIndexOf(NodeId id) const { return activePath().indexOf(id); }

ChatTree::NodeId ChatTree::lastOnPath() const {
    const QList<NodeId> path = activePath();
    return path.isEmpty() ? kRoot : path.last();
}

QList<ChatTree::NodeId> ChatTree::versionsOf(NodeId id) const {
    if (!valid(id)) return {};
    const NodeId parent = node(id).parent;
    return parent == kRoot ? m_rootChildren : node(parent).children;
}

int ChatTree::versionIndex(NodeId id) const { return versionsOf(id).indexOf(id); }
int ChatTree::versionCount(NodeId id) const { return versionsOf(id).size(); }

void ChatTree::activate(NodeId id) {
    if (!valid(id)) return;
    const NodeId parent = node(id).parent;
    if (parent == kRoot) m_rootActive = id;
    else node(parent).activeChild = id;
}

QList<QJsonObject> ChatTree::messages(int pathCount) const {
    QList<QJsonObject> out;
    const QList<NodeId> path = activePath();
    const int count = pathCount < 0 ? path.size() : std::min<int>(pathCount, path.size());
    for (int i = 0; i < count; ++i)
        out.append(QJsonObject{{"role", node(path.at(i)).role}, {"content", node(path.at(i)).content}});
    return out;
}

void ChatTree::addNote(NodeId id, const QString &text) {
    if (valid(id)) node(id).notes.append(text);
}

QStringList ChatTree::notes(NodeId id) const { return valid(id) ? node(id).notes : QStringList(); }

void ChatTree::setCompaction(NodeId id, const Compaction &compaction) {
    if (valid(id)) node(id).compaction = compaction;
}

ChatTree::Compaction ChatTree::compaction(int pathCount) const {
    const QList<NodeId> path = activePath();
    const int count = pathCount < 0 ? path.size() : std::min<int>(pathCount, path.size());
    for (int i = count - 1; i >= 0; --i)
        if (node(path.at(i)).compaction.covered > 0) return node(path.at(i)).compaction;
    return Compaction();
}

bool ChatTree::isEmpty() const { return m_nodes.isEmpty(); }

void ChatTree::clear() {
    m_nodes.clear();
    m_rootChildren.clear();
    m_rootActive = kRoot;
}
