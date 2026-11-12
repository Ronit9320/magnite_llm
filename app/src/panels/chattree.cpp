#include "panels/chattree.h"

#include <algorithm>

#include <QJsonArray>
#include <QSet>

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
    m_knowledge = {};
    m_nodes.clear();
    m_rootChildren.clear();
    m_rootActive = kRoot;
}

QJsonObject ChatTree::toJson() const {
    auto ids = [](const QList<NodeId> &list) {
        QJsonArray array;
        for (NodeId id : list) array.append(id);
        return array;
    };
    QJsonArray nodes;
    for (int i = 0; i < m_nodes.size(); ++i) {
        const Node &n = m_nodes.at(i);
        QJsonArray notes;
        for (const QString &note : n.notes) notes.append(note);
        nodes.append(QJsonObject{{"id", i + 1},
                                 {"parent", n.parent},
                                 {"role", n.role},
                                 {"content", n.content},
                                 {"children", ids(n.children)},
                                 {"activeChild", n.activeChild},
                                 {"notes", notes},
                                 {"citations", n.citations},
                                 {"compaction", QJsonObject{{"summary", n.compaction.summary}, {"covered", n.compaction.covered}}}});
    }
    return QJsonObject{{"nodes", nodes}, {"rootChildren", ids(m_rootChildren)}, {"rootActive", m_rootActive}, {"knowledge", m_knowledge}};
}

namespace {
// A JSON number that is a whole number.
bool wholeNumber(const QJsonValue &value, int &out) {
    if (!value.isDouble()) return false;
    const double d = value.toDouble();
    if (d < 0 || d > 1e9 || d != double(int(d))) return false;
    out = int(d);
    return true;
}
bool idList(const QJsonValue &value, QList<ChatTree::NodeId> &out) {
    if (!value.isArray()) return false;
    for (const QJsonValue &item : value.toArray()) {
        int id = 0;
        if (!wholeNumber(item, id)) return false;
        out.append(id);
    }
    return true;
}
}  // namespace

std::optional<ChatTree> ChatTree::fromJson(const QJsonObject &json) {
    if (!json.value("nodes").isArray()) return std::nullopt;
    const QJsonArray nodes = json.value("nodes").toArray();
    const int count = nodes.size();
    ChatTree tree;
    tree.m_knowledge = json.value("knowledge").toObject();
    tree.m_nodes.resize(count);
    for (int i = 0; i < count; ++i) {
        if (!nodes.at(i).isObject()) return std::nullopt;
        const QJsonObject o = nodes.at(i).toObject();
        Node &n = tree.m_nodes[i];
        int id = 0, parent = 0, active = 0, covered = 0;
        if (!wholeNumber(o.value("id"), id) || id != i + 1) return std::nullopt;
        if (!wholeNumber(o.value("parent"), parent) || parent > count || parent == id) return std::nullopt;
        const QString role = o.value("role").toString();
        if (!o.value("role").isString() || (role != "user" && role != "assistant" && role != "system")) return std::nullopt;
        if (!o.value("content").isString()) return std::nullopt;
        if (!idList(o.value("children"), n.children) || !wholeNumber(o.value("activeChild"), active)) return std::nullopt;
        if (!o.value("notes").isArray()) return std::nullopt;
        for (const QJsonValue &note : o.value("notes").toArray()) {
            if (!note.isString()) return std::nullopt;
            n.notes.append(note.toString());
        }
        const QJsonValue compaction = o.value("compaction");
        if (!compaction.isObject() || !compaction.toObject().value("summary").isString() ||
            !wholeNumber(compaction.toObject().value("covered"), covered))
            return std::nullopt;
        n.parent = parent;
        n.role = role;
        n.content = o.value("content").toString();
        n.citations = o.value("citations").toArray();
        n.activeChild = active;
        n.compaction = Compaction{compaction.toObject().value("summary").toString(), covered};
    }
    int rootActive = 0;
    if (!idList(json.value("rootChildren"), tree.m_rootChildren) || !wholeNumber(json.value("rootActive"), rootActive)) return std::nullopt;
    tree.m_rootActive = rootActive;

    // Every child list names real nodes whose parent field points back, and each node is listed exactly once.
    QSet<NodeId> listed;
    auto checkChildren = [&](const QList<NodeId> &children, NodeId owner, NodeId active) {
        for (NodeId child : children) {
            if (child < 1 || child > count || tree.node(child).parent != owner || listed.contains(child)) return false;
            listed.insert(child);
        }
        return children.isEmpty() ? active == kRoot : children.contains(active);
    };
    if (!checkChildren(tree.m_rootChildren, kRoot, tree.m_rootActive)) return std::nullopt;
    for (int i = 0; i < count; ++i)
        if (!checkChildren(tree.m_nodes.at(i).children, i + 1, tree.m_nodes.at(i).activeChild)) return std::nullopt;
    if (listed.size() != count) return std::nullopt;

    // Everything must be reachable from the root (no cycles, nothing orphaned); a snapshot cannot cover
    // more messages than exist up to the message that holds it.
    int reached = 0;
    QList<QPair<NodeId, int>> stack;
    for (NodeId id : tree.m_rootChildren) stack.append({id, 1});
    while (!stack.isEmpty()) {
        const auto [id, depth] = stack.takeLast();
        ++reached;
        if (reached > count) return std::nullopt;
        if (tree.node(id).compaction.covered > depth) return std::nullopt;
        for (NodeId child : tree.node(id).children) stack.append({child, depth + 1});
    }
    if (reached != count) return std::nullopt;
    return tree;
}
