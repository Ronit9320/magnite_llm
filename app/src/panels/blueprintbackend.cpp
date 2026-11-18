#include "panels/blueprintpreview.h"

#include "backend_client.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>
#include <QTimer>
#include <QUrl>

namespace {
using Result = BlueprintPreview::Result;

QJsonObject findById(const QJsonArray &entries, const QString &id) {
    for (const auto &value : entries) {
        const auto entry = value.toObject();
        if (entry["id"].toString() == id) return entry;
    }
    return {};
}

void upsert(QJsonArray &entries, const QJsonObject &entry) {
    for (int i = 0; i < entries.size(); ++i) {
        if (entries.at(i).toObject()["id"] != entry["id"]) continue;
        entries.replace(i, entry);
        return;
    }
    entries.prepend(entry);
}

QString segment(const QString &id) { return QString::fromLatin1(QUrl::toPercentEncoding(id)); }
QString blueprintPath(const QString &id) { return "/blueprints/" + segment(id); }
QString runPath(const QString &id) { return blueprintPath("runs") + "/" + segment(id); }

QString issuesText(const QJsonArray &issues) {
    QStringList messages;
    for (const auto &value : issues) {
        const auto issue = value.toObject();
        QString location = issue["node_id"].toString();
        const auto path = issue["path"].toString();
        if (!path.isEmpty()) location += (location.isEmpty() ? QString{} : QString(" · ")) + path;
        if (location.isEmpty()) {
            QStringList fields;
            for (const auto &field : issue["loc"].toArray())
                fields << (field.isDouble() ? QString::number(field.toInt()) : field.toString());
            location = fields.join('.');
        }
        const auto text = issue["message"].toString(issue["msg"].toString());
        if (!text.isEmpty()) messages << (location.isEmpty() ? text : location + ": " + text);
    }
    return messages.join('\n');
}

QString errorText(int status, const QJsonDocument &doc) {
    const auto detail = doc.object()["detail"];
    auto message = detail.isArray() ? issuesText(detail.toArray()) : detail.toString();
    if (message.isEmpty()) message = doc.object()["error"].toString();
    if (!message.isEmpty()) return message;
    if (!status) return "The local service is disconnected. Refresh status before retrying.";
    return QString("The local service returned HTTP %1.").arg(status);
}

QString titleCase(const QString &id) {
    auto title = id;
    title.replace('_', ' ').replace('-', ' ');
    if (!title.isEmpty()) title[0] = title[0].toUpper();
    return title;
}

QJsonObject adapterPort() {
    return {{"id", "adapter"}, {"name", "LoRA adapter"}, {"type", "adapter-ref"},
            {"schema", QJsonObject{{"type", "string"}}}};
}

void addAdapterInput(QJsonObject &node) {
    auto ports = node["in"].toArray();
    if (findById(ports, "adapter").isEmpty()) ports.append(adapterPort());
    node["in"] = ports;
}

QJsonObject apiNode(const QJsonObject &node) {
    const auto kind = node["kind"].toString();
    const auto op = BlueprintPreview::operationId(node);
    QJsonObject body{{"id", node["id"]}, {"kind", kind}, {"operation", op}, {"name", node["name"]}};
    for (const auto &side : {QString("in"), QString("out")}) {
        QJsonObject ports;
        for (const auto &value : node[side].toArray()) {
            const auto port = value.toObject();
            if (side == "in" && port["type"] == "adapter-ref") continue;
            ports[port["id"].toString()] = port["schema"];
        }
        body[side == "in" ? "inputs" : "outputs"] = ports;
    }
    if (kind == "model" || kind == "reasoning") {
        QJsonObject binding{{"model_id", node["model"].toString()}};
        if (!node["adapter"].toString().isEmpty()) binding["adapter_id"] = node["adapter"];
        body["binding"] = binding;
        body["max_tokens"] = node["maxTokens"].toInt(kind == "reasoning" ? 1024 : 256);
        body["response"] = node["response"].toString("json");
        body["system_prompt"] = node["prompt"].toString("Return only the requested JSON value.");
    } else if (kind == "adapter") {
        body["adapter_id"] = node["adapter"];
    } else if (kind == "agent") {
        body["instructions"] = node["setting"].toString();
    } else if (kind == "dataset") {
        if (node["dataset"].toString().isEmpty()) {
            const auto rows = QJsonDocument::fromJson(node["setting"].toString().toUtf8());
            body["binding"] = QJsonObject{{"source", "inline"}, {"rows", rows.isArray() ? QJsonValue(rows.array()) : QJsonValue()}};
        } else {
            body["binding"] = QJsonObject{{"source", "local"}, {"dataset_id", node["dataset"]}};
        }
    } else if (op == "field") {
        body["parameters"] = QJsonObject{{"field", node["setting"].toString()}};
    } else if (op == "constant") {
        body["parameters"] = QJsonObject{{"value", BlueprintPreview::parseJsonValue(node["setting"].toString())}};
    }
    return body;
}

QJsonObject apiGraph(const QJsonObject &graph) {
    QJsonArray nodes, edges;
    for (const auto &value : graph["nodes"].toArray()) nodes.append(apiNode(value.toObject()));
    for (const auto &value : graph["edges"].toArray()) {
        const auto edge = value.toObject();
        edges.append(QJsonObject{{"source", QJsonObject{{"node", edge["from"]}, {"port", edge["out"]}}},
                                 {"target", QJsonObject{{"node", edge["to"]}, {"port", edge["in"]}}}});
    }
    return {{"version", 1}, {"name", graph["name"]}, {"nodes", nodes}, {"edges", edges}};
}

QJsonObject editorNode(const QJsonObject &node, const QJsonObject &layout, int index) {
    const auto id = node["id"].toString();
    const auto binding = node["binding"].toObject();
    QJsonObject result{{"id", id}, {"name", node["name"].toString(titleCase(id))},
        {"kind", node["kind"]}, {"operation", node["operation"]},
        {"x", layout["x"].toInt((index % 4) * 222)}, {"y", layout["y"].toInt((index / 4) * 178)}};
    for (const auto &side : {QString("in"), QString("out")}) {
        const auto ports = node[side == "in" ? "inputs" : "outputs"].toObject();
        QJsonArray entries;
        // Keep the desktop port order when the revision came from this editor.
        QStringList keys;
        const auto local = layout[side].toArray();
        for (const auto &entry : local) {
            const auto key = entry.toObject()["id"].toString();
            if (ports.contains(key) && !keys.contains(key)) keys << key;
        }
        for (const auto &key : ports.keys())
            if (!keys.contains(key)) keys << key;
        for (const auto &key : keys) {
            const auto schema = ports[key].toObject();
            const auto type = schema["type"].toString();
            const auto entry = findById(local, key);
            entries.append(QJsonObject{{"id", key}, {"name", entry["name"].toString(titleCase(key))},
                {"schema", schema}, {"type", type == "string" ? QString("text") : type}});
        }
        result[side] = entries;
    }
    if (binding.contains("model_id")) {
        result["model"] = binding["model_id"];
        result["adapter"] = binding["adapter_id"].toString();
        result["maxTokens"] = node["max_tokens"];
        result["response"] = node["response"];
        result["prompt"] = node["system_prompt"];
        addAdapterInput(result);
    } else if (binding["source"] == "local") {
        result["dataset"] = binding["dataset_id"];
        result["setting"] = "[]";
    } else if (binding["source"] == "inline") {
        result["setting"] = BlueprintPreview::jsonText(binding["rows"]);
    }
    if (node["operation"] == "field") result["setting"] = node["parameters"].toObject()["field"];
    if (node["operation"] == "constant")
        result["setting"] = BlueprintPreview::jsonText(node["parameters"].toObject()["value"]);
    if (node["kind"] == "agent") result["setting"] = node["instructions"].toString();
    if (node["kind"] == "adapter") {
        result["adapter"] = node["adapter_id"];
        result["out"] = QJsonArray{adapterPort()};
    }
    return result;
}

QJsonObject editorGraph(const QJsonObject &revision, const QJsonObject &layout = {}) {
    const auto doc = revision["blueprint"].toObject();
    const auto previous = layout["nodes"].toArray();
    QJsonArray nodes, edges;
    for (const auto &value : doc["nodes"].toArray()) {
        const auto node = value.toObject();
        nodes.append(editorNode(node, findById(previous, node["id"].toString()), nodes.size()));
    }
    for (const auto &value : doc["edges"].toArray()) {
        const auto edge = value.toObject();
        const auto from = edge["source"].toObject();
        const auto to = edge["target"].toObject();
        edges.append(QJsonObject{{"from", from["node"]}, {"out", from["port"]}, {"to", to["node"]}, {"in", to["port"]}});
    }
    return {{"id", revision["id"]}, {"revision", revision["revision"]}, {"name", doc["name"]},
        {"description", layout["description"].toString()}, {"nodes", nodes}, {"edges", edges}, {"dirty", false}};
}

qint64 timestamp(const QJsonValue &date) {
    return QDateTime::fromString(date.toString(), Qt::ISODateWithMs).toMSecsSinceEpoch();
}

bool active(const QJsonObject &record) {
    const auto status = record["status"].toString();
    return status == "queued" || status == "running" || status == "waiting";
}

QJsonObject runSummary(const QJsonObject &raw) {
    QJsonObject record{{"id", raw["id"]}, {"blueprintId", raw["blueprint_id"]}, {"name", raw["name"]},
        {"revision", raw["revision"]}, {"target", raw["test_node"].toString()}, {"status", raw["status"]},
        {"source", raw["surface"] == "chat" ? "Local chat" : "Blueprints"},
        {"created", timestamp(raw["created_at"])}};
    if (raw["finished_at"].isString()) record["ended"] = timestamp(raw["finished_at"]);
    if (raw["error"].isObject()) record["error"] = issuesText(QJsonArray{raw["error"]});
    if (raw["tool_chat"].isBool()) record["toolChat"] = raw["tool_chat"];
    return record;
}

QJsonObject displayRun(const QJsonObject &raw, const QJsonArray &approvals) {
    auto record = runSummary(raw);
    const auto snapshot = raw["snapshot"].toObject();
    const auto graph = snapshot["blueprint"].toObject();
    const auto request = snapshot["request"].toObject();
    const auto legacyToolChat = raw["surface"] == "chat" && graph["name"] == "Tool chat" &&
        !snapshot["tool_model"].toObject().isEmpty() && !findById(graph["nodes"].toArray(), "assistant").isEmpty() &&
        request["inputs"].toObject()["messages"].isArray();
    const bool toolChat = raw["tool_chat"].toBool(legacyToolChat);
    record["details"] = true;
    record["result"] = toolChat ? raw["results"].toObject()["reply"].toObject()["text"] : raw["results"];
    record["toolChat"] = toolChat;
    auto supplied = request["inputs"].toObject();
    if (record["target"].isString() && !record["target"].toString().isEmpty() && supplied.isEmpty())
        supplied = snapshot["test_inputs"].toObject();
    record["supplied"] = supplied;
    const auto prompt = supplied.value("messages").toArray();
    if (toolChat && !prompt.isEmpty()) {
        record["input"] = prompt.last().toObject()["content"].toString();
        record["messages"] = prompt;
        record["model"] = BlueprintPreview::modelLabel(snapshot["tool_model"].toObject()["model"].toString());
    } else {
        const auto single = supplied.size() == 1 ? supplied.begin().value() : QJsonValue();
        if (single.isString()) record["input"] = single.toString();
        else if (!supplied.isEmpty()) record["input"] = BlueprintPreview::jsonText(supplied);
    }
    QJsonArray decisions, steps;
    QSet<QString> paused;
    for (const auto &value : approvals) {
        const auto item = value.toObject();
        const auto action = item["action"].toObject();
        const bool pending = item["status"] == "pending" && !action.isEmpty() && active(record);
        const auto status = item["status"].toString();
        QJsonObject decision{{"id", item["id"]}, {"fingerprint", item["fingerprint"]}, {"stepId", item["node_id"]},
            {"type", item["kind"] == "command" ? "Run command" : item["kind"] == "read" ? "Read file or folder" : "Open target"},
            {"status", pending ? QString("waiting") : status == "pending" ? QString("unavailable") : status},
            {"expires", timestamp(item["expires_at"])}, {"outcome", item["outcome"]}};
        if (action.contains("executable")) {
            decision["executable"] = action["executable"];
            decision["args"] = action["arguments"];
            decision["cwd"] = action["working_directory"];
        } else if (action.contains("target")) {
            decision["target"] = action["target"];
        }
        if (pending) paused.insert(item["node_id"].toString());
        decisions.append(decision);
    }
    if (!paused.isEmpty()) record["status"] = "waiting";
    int cursor = -1;
    const auto nodes = graph["nodes"].toArray();
    for (const auto &value : raw["steps"].toArray()) {
        const auto step = value.toObject();
        const auto id = step["node_id"].toString();
        auto entry = editorNode(findById(nodes, id), {}, 0);
        const auto status = paused.contains(id) ? QString("waiting") : step["status"].toString();
        if (status == "running" || status == "waiting" || status == "failed") cursor = steps.size();
        entry["status"] = status;
        entry["inputs"] = step["inputs"];
        entry["output"] = step["outputs"];
        if (step["duration_ms"].isDouble()) entry["duration"] = qRound64(step["duration_ms"].toDouble());
        if (step["error"].isObject()) entry["error"] = issuesText(QJsonArray{step["error"]});
        steps.append(entry);
    }
    record["steps"] = steps;
    record["cursor"] = cursor;
    record["decisions"] = decisions;
    return record;
}
}

bool BlueprintPreview::connected() const { return m_backend && m_backend->isReady(); }

void BlueprintPreview::request(const QString &method, const QString &path, const QJsonObject &body, Response done) {
    if (!connected()) {
        done(0, {});
        return;
    }
    const auto accept = [self = QPointer(this), epoch = m_session, done = std::move(done)]
        (int status, const QJsonDocument &doc) {
        if (!self) return;
        if (self->m_session == epoch) done(status, doc);
        else done(0, {});
    };
    if (method == "GET") m_backend->get(path, accept);
    else if (method == "POST") m_backend->post(path, body, accept);
    else if (method == "PUT") m_backend->put(path, body, accept);
    else if (method == "DELETE") m_backend->remove(path, accept);
}

QJsonArray BlueprintPreview::saved() const {
    if (!live()) return m_saved;
    auto result = m_published;
    for (const auto &value : m_saved) {
        auto editor = value.toObject();
        const auto server = findById(m_published, editor["id"].toString());
        // A clean local copy keeps the layout; only the server can list it as saved.
        if (editor["dirty"].toBool() || (!server.isEmpty() && server["revision"] == editor["revision"]))
            upsert(result, editor);
    }
    return result;
}

void BlueprintPreview::refreshSaved() {
    if (connected()) fetchSaved(0, {}, ++m_savedRequest);
}

void BlueprintPreview::fetchSaved(int offset, QJsonArray entries, quint64 generation) {
    request("GET", QString("/blueprints?limit=100&offset=%1").arg(offset), {},
        [this, offset, generation, entries = std::move(entries)](int status, const QJsonDocument &doc) mutable {
        if (generation != m_savedRequest) return;
        if (status != 200) {
            m_serviceError = errorText(status, doc);
            emit serviceChanged();
            return;
        }
        const auto page = doc.array();
        for (const auto &value : page) entries.append(value);
        if (page.size() == 100) { fetchSaved(offset + 100, std::move(entries), generation); return; }
        m_serviceError.clear();
        m_published = entries;
        emit savedChanged();
        emit serviceChanged();
    });
}

void BlueprintPreview::getBlueprint(const QString &id, bool publishedOnly, Result done, qint64 revision) {
    const auto draft = blueprint(id);
    if (!publishedOnly && !draft.isEmpty()) {
        const auto published = findById(m_published, id);
        if (draft["dirty"].toBool() || draft["revision"] == published["revision"]) {
            done(draft, {});
            return;
        }
    }
    auto path = blueprintPath(id);
    if (revision > 0) path += QString("?revision=%1").arg(revision);
    request("GET", path, {}, [done, draft](int status, const QJsonDocument &doc) {
        if (status != 200) { done({}, errorText(status, doc)); return; }
        done(editorGraph(doc.object(), draft), {});
    });
}

void BlueprintPreview::reviseLocal(const QJsonObject &revision, QJsonObject editor, Result done) {
    auto updated = editorGraph(revision, editor);
    const auto id = revision["id"].toString();
    upsert(m_published, QJsonObject{{"id", id}, {"revision", revision["revision"]}, {"name", updated["name"]}});
    if (!saveBlueprint(updated)) {
        emit savedChanged();
        done(updated, "The revision was saved, but the editor layout could not be stored.");
    } else {
        done(updated, {});
    }
    refreshSaved();
}

void BlueprintPreview::publish(const QJsonObject &graph, Result done) {
    request("PUT", blueprintPath(graph["id"].toString()) + QString("?expected_revision=%1").arg(graph["revision"].toInteger()),
        apiGraph(graph), [this, graph, done](int status, const QJsonDocument &doc) {
        if (status != 200) {
            if (status == 409 || status == 404) refreshSaved();
            done({}, errorText(status, doc));
            return;
        }
        reviseLocal(doc.object(), graph, done);
    });
}

void BlueprintPreview::removeBlueprint(const QString &id, Result done) {
    const auto published = findById(m_published, id);
    const auto removeLocal = [this, done, id] {
        if (!blueprint(id).isEmpty() && !deleteBlueprint(id)) {
            done({}, "The local draft could not be removed.");
            return;
        }
        done({}, {});
    };
    if (published.isEmpty()) {
        // Offline cache entries may point to a revision that still exists.
        if (blueprint(id)["revision"].toInteger() > 0 && !connected()) {
            done({}, "Reconnect to the local service to delete this blueprint.");
            return;
        }
        removeLocal();
        return;
    }
    const auto rev = published["revision"].toInteger();
    request("DELETE", blueprintPath(id) + QString("?expected_revision=%1").arg(rev), {},
        [this, id, done, removeLocal](int status, const QJsonDocument &doc) {
        if (status != 204 && status != 404) {
            done({}, errorText(status, doc));
            return;
        }
        for (int i = 0; i < m_published.size(); ++i) {
            if (m_published.at(i).toObject()["id"] != id) continue;
            m_published.removeAt(i);
            break;
        }
        emit savedChanged();
        removeLocal();
        refreshSaved();
    });
}

void BlueprintPreview::validateLive(const QJsonObject &graph, Result done) {
    request("POST", "/blueprints/capabilities", apiGraph(graph), [done](int status, const QJsonDocument &doc) {
        done(doc.object(), status == 200 ? QString{} : errorText(status, doc));
    });
}

QJsonObject BlueprintPreview::inputContracts(const QJsonObject &graph, const QString &target) {
    QJsonObject ports;
    for (const auto &value : graph["nodes"].toArray()) {
        const auto node = value.toObject();
        const auto id = node["id"].toString();
        const bool input = node["kind"] == "input";
        if (!target.isEmpty() && target != id) continue;
        if (target.isEmpty() && !input) continue;
        for (const auto &value : node[input ? "out" : "in"].toArray()) {
            const auto port = value.toObject();
            if (port["type"] == "adapter-ref") continue;
            ports[target.isEmpty() ? id : port["id"].toString()] = port["schema"];
        }
    }
    return ports;
}

void BlueprintPreview::runLive(const QJsonObject &graph, const QJsonValue &input, const QString &target,
    bool chat, Result done) {
    const auto id = graph["id"].toString();
    QString path;
    QJsonObject body;
    if (!target.isEmpty()) {
        auto node = findById(graph["nodes"].toArray(), target);
        if (node.isEmpty()) { done({}, "Choose a node to test."); return; }
        if (findById(node["in"].toArray(), "adapter")["type"] == "adapter-ref") {
            for (const auto &value : graph["edges"].toArray()) {
                const auto edge = value.toObject();
                if (edge["to"] != target || edge["in"] != "adapter") continue;
                const auto adapter = findById(graph["nodes"].toArray(), edge["from"].toString())["adapter"].toString();
                if (adapter.isEmpty()) { done({}, "Select a trained adapter on the connected Adapter node."); return; }
                node["adapter"] = adapter;
                break;
            }
        }
        path = blueprintPath(id) + "/node-tests";
        body = {{"name", graph["name"]}, {"node", apiNode(node)}, {"inputs", input}};
    } else {
        const auto revision = graph["revision"].toInteger();
        if (!revision || graph["dirty"].toBool()) {
            done({}, "Save a revision before running the full blueprint.");
            return;
        }
        path = blueprintPath(id) + "/runs" + (chat ? QString("?surface=chat") : QString{});
        body = {{"revision", revision}, {"mode", "local"}, {"inputs", input}};
    }
    submitRun(path, body, std::move(done));
}

void BlueprintPreview::runToolTask(const QJsonArray &messages, Result done) {
    submitRun("/inference/tool-runs", {{"messages", messages}}, std::move(done));
}

void BlueprintPreview::submitRun(const QString &path, const QJsonObject &body, Result done) {
    request("POST", path, body, [this, done = std::move(done)](int status, const QJsonDocument &doc) {
        if (status != 202) { done({}, errorText(status, doc)); return; }
        auto record = runSummary(doc.object());
        rememberRun(record);
        loadRun(record["id"].toString());
        done(record, {});
    });
}

QJsonArray BlueprintPreview::recentRuns() const {
    QJsonArray result;
    for (const auto &value : m_runs) {
        result.append(value);
        if (result.size() >= 50) break;
    }
    return result;
}

void BlueprintPreview::refreshHistory(const QString &id) {
    if (live() && connected()) refreshRuns(id.isEmpty() ? "?limit=50" : "?blueprint_id=" + segment(id));
}

void BlueprintPreview::refreshRuns(const QString &query, bool openChat) {
    request("GET", "/blueprints/runs" + query, {}, [this, openChat](int status, const QJsonDocument &doc) {
        if (status != 200) return;
        for (const auto &value : doc.array()) {
            const auto summary = runSummary(value.toObject());
            const auto id = summary["id"].toString();
            const auto existing = run(id);
            if (existing.isEmpty()) rememberRun(summary);
            if ((openChat && !existing["details"].toBool()) || active(summary) || active(existing)) loadRun(id);
        }
    });
}

void BlueprintPreview::rememberRun(const QJsonObject &record) {
    const auto id = record["id"].toString();
    if (record == run(id)) return;
    for (int i = 0; i < m_runs.size(); ++i) {
        if (m_runs.at(i).toObject()["id"] != id) continue;
        m_runs.removeAt(i);
        break;
    }
    int index = 0;
    while (index < m_runs.size() && m_runs.at(index).toObject()["created"].toInteger() > record["created"].toInteger())
        ++index;
    m_runs.insert(index, record);
    emit runChanged(id);
}

void BlueprintPreview::loadRun(const QString &id, Result done) {
    if (id.isEmpty()) return;
    if (!connected()) {
        if (done) done(run(id), "The local service is disconnected.");
        return;
    }
    if (m_refreshingRuns.contains(id)) {
        if (done) m_refreshingRuns[id].append(done);
        else m_dirtyRuns.insert(id);
        return;
    }
    m_refreshingRuns.insert(id, done ? QList<Result>{done} : QList<Result>{});
    const auto epoch = m_session;
    const auto finish = [this, id, epoch](QJsonObject record, const QString &error) {
        if (epoch != m_session) return;
        if (record.isEmpty() && !error.isEmpty()) record = run(id);
        if (!record.isEmpty()) {
            // The broker redacts settled actions. Keep details already reviewed in this desktop session.
            const auto previous = run(id)["decisions"].toArray();
            auto decisions = record["decisions"].toArray();
            for (int index = 0; index < decisions.size(); ++index) {
                auto decision = decisions.at(index).toObject();
                if (decision["status"] == "waiting" || decision["status"] == "unavailable") continue;
                const auto old = findById(previous, decision["id"].toString());
                for (const auto *field : {"executable", "args", "cwd", "target"})
                    if (old.contains(field)) decision[field] = old[field];
                decisions.replace(index, decision);
            }
            if (!decisions.isEmpty()) record["decisions"] = decisions;
            const auto message = m_actionErrors.value(id, error);
            if (message.isEmpty()) record.remove("actionError");
            else record["actionError"] = message;
        }
        if (!record.isEmpty()) rememberRun(record);
        const auto waiters = m_refreshingRuns.take(id);
        for (const auto &waiter : waiters) waiter(record, error);
        if (m_dirtyRuns.remove(id)) loadRun(id);
        if (active(run(id))) connectRun(id);
        else m_cursors.remove(id);
    };
    request("GET", runPath(id), {}, [this, id, finish](int status, const QJsonDocument &doc) {
        if (status != 200) { finish({}, errorText(status, doc)); return; }
        const auto raw = doc.object();
        const auto nodes = raw["snapshot"].toObject()["blueprint"].toObject()["nodes"].toArray();
        for (const auto &value : nodes) {
            if (value.toObject()["kind"] != "tool" || raw["mode"] != "local") continue;
            request("GET", runPath(id) + "/approvals", {}, [raw, finish](int status, const QJsonDocument &doc) {
                if (status != 200) {
                    finish({}, errorText(status, doc));
                    return;
                }
                finish(displayRun(raw, doc.array()), {});
            });
            return;
        }
        finish(displayRun(raw, {}), {});
    });
}

void BlueprintPreview::connectRun(const QString &id) {
    // Leave network slots available for decisions and for ordinary chat.
    if (!connected() || m_streams.contains(id) || m_streams.size() >= 2) return;
    m_streams.insert(id);
    const auto epoch = m_session;
    m_backend->streamGet(runPath(id) + "/events", m_cursors.value(id),
        [self = QPointer(this), id, epoch](qint64 cursor) {
            if (!self || epoch != self->m_session || cursor <= self->m_cursors.value(id)) return;
            self->m_cursors[id] = cursor;
            self->loadRun(id);
        }, [self = QPointer(this), id, epoch](int status) {
            if (!self || epoch != self->m_session) return;
            QTimer::singleShot(status == 200 ? 0 : 1800, self, [self, id, epoch] {
                if (epoch != self->m_session) return;
                self->m_streams.remove(id);
                self->loadRun(id);
            });
        });
}

void BlueprintPreview::decideLive(const QString &id, bool approved, const QJsonObject &displayed) {
    const auto approval = displayed["id"].toString();
    const auto fingerprint = displayed["fingerprint"].toString();
    if (!connected() || approval.isEmpty() || fingerprint.isEmpty() || displayed["status"] != "waiting" || m_actions.contains(id))
        return;
    m_actions.insert(id);
    request("POST", runPath(id) + "/approvals/" + segment(approval) + "/decision",
        {{"decision", approved ? "approve_once" : "deny"}, {"fingerprint", fingerprint}},
        [this, id](int status, const QJsonDocument &doc) {
            m_actions.remove(id);
            m_actionErrors.remove(id);
            if (status != 200) m_actionErrors.insert(id, errorText(status, doc));
            loadRun(id);
        });
}

void BlueprintPreview::cancelLive(const QString &id) {
    if (m_actions.contains(id)) return;
    m_actions.insert(id);
    request("POST", runPath(id) + "/cancel", {}, [this, id](int status, const QJsonDocument &doc) {
        m_actions.remove(id);
        m_actionErrors.remove(id);
        if (status != 200) m_actionErrors.insert(id, errorText(status, doc));
        loadRun(id);
    });
}

void BlueprintPreview::refreshOptions() {
    request("GET", "/blueprints/checkpoints", {}, [this](int status, const QJsonDocument &doc) {
        if (status != 200) return;
        if (m_fixtures.value("models").toArray() == doc.array()) return;
        m_fixtures["models"] = doc.array();
        emit optionsChanged();
    });
    request("GET", "/datasets", {}, [this](int status, const QJsonDocument &doc) {
        if (status != 200) return;
        if (m_datasets == doc.array()) return;
        m_datasets = doc.array();
        emit optionsChanged();
    });
    request("GET", "/adapters", {}, [this](int status, const QJsonDocument &doc) {
        if (status != 200) return;
        QJsonArray ready;
        for (const auto &value : doc.array()) {
            const auto adapter = value.toObject();
            if (adapter["status"] == "ready")
                ready.append(QJsonObject{{"id", adapter["id"]}, {"name", adapter["name"]},
                                         {"base_model", adapter["base_model"]}});
        }
        if (m_fixtures["adapters"].toArray() == ready) return;
        m_fixtures["adapters"] = ready;
        emit optionsChanged();
    });
}

void BlueprintPreview::checkpoint(const QString &model, Result done) {
    request("GET", "/blueprints/checkpoints/" + segment(model), {}, [done](int status, const QJsonDocument &doc) {
        done(doc.object(), status == 200 ? QString{} : errorText(status, doc));
    });
}

QString BlueprintPreview::modelLabel(const QString &reference) {
    // Mirrors the backend's label: a Hugging Face cache snapshot, GGUF file, or repo ID.
    static const QRegularExpression cached(R"(models--[^/\\]*?--([^/\\]+)[/\\]snapshots)");
    if (const auto match = cached.match(reference); match.hasMatch()) return match.captured(1);
    const auto name = reference.section(QRegularExpression(R"([/\\])"), -1);
    return name.endsWith(".gguf", Qt::CaseInsensitive) ? name.chopped(5) : name.isEmpty() ? reference : name;
}

void BlueprintPreview::toolCapability(Result done) {
    request("GET", "/inference/tool-runs/capabilities", {}, [done](int status, const QJsonDocument &doc) {
        const auto result = doc.object();
        done(result, status != 200 || !result["available"].toBool() ? errorText(status, doc) : QString{});
    });
}

void BlueprintPreview::train(const QJsonObject &graph, const QString &node, const QJsonArray &examples,
    const QJsonObject &settings, Result done) {
    request("POST", blueprintPath(graph["id"].toString()) + "/nodes/" + segment(node) + "/training",
        {{"revision", graph["revision"]}, {"examples", examples}, {"settings", settings}},
        [done](int status, const QJsonDocument &doc) {
            done(doc.object(), status == 202 ? QString{} : errorText(status, doc));
        });
}

void BlueprintPreview::jobs(const QString &blueprint, const QString &node, Result done) {
    request("GET", "/training/jobs", {}, [blueprint, node, done](int status, const QJsonDocument &doc) {
        QJsonArray options;
        for (const auto &value : doc.array()) {
            const auto job = value.toObject();
            const auto origin = job["config"].toObject()["blueprint_node"].toObject();
            if (origin["blueprint_id"] == blueprint && origin["node_id"] == node)
                options.append(job);
        }
        done(QJsonObject{{"jobs", options}}, status == 200 ? QString{} : errorText(status, doc));
    });
}

void BlueprintPreview::bindTraining(const QJsonObject &graph, const QString &node, const QString &job, Result done) {
    request("PUT", blueprintPath(graph["id"].toString()) + "/nodes/" + segment(node) + "/adapter",
        {{"revision", graph["revision"]}, {"training_job_id", job}},
        [this, graph, done](int status, const QJsonDocument &doc) {
            if (status != 200) { done({}, errorText(status, doc)); return; }
            reviseLocal(doc.object(), graph, done);
        });
}
