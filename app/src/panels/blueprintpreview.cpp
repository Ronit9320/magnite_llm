#include "panels/blueprintpreview.h"
#include "textformat.h"
#include "backend_client.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QSettings>
#include <QTimer>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <utility>

namespace {
constexpr auto savedKey = "blueprintsPreview/saved";
constexpr auto nodeLibraryKey = "blueprintsPreview/nodeLibrary";
constexpr auto runsKey = "blueprintsPreview/runs";
constexpr qint64 approvalMs = 45000;

QJsonObject findById(const QJsonArray &entries, const QString &id) {
    for (const auto &value : entries) {
        const auto object = value.toObject();
        if (object["id"].toString() == id) return object;
    }
    return {};
}

QStringList strings(const QJsonArray &values) {
    QStringList result;
    for (const auto &value : values) result.append(value.toString());
    return result;
}

QJsonObject price(QJsonObject fields) {
    const double subtotal = std::round(fields["quantity"].toDouble() * fields["unit_price"].toDouble() * 100) / 100;
    const int shipping = subtotal < 50 ? 5 : 0;
    fields["subtotal"] = subtotal;
    fields["shipping"] = shipping;
    fields["total"] = subtotal + shipping;
    return fields;
}

bool writeEntries(const char *key, const QJsonArray &entries, bool persist) {
    if (!persist) return true;
    QSettings settings;
    settings.setValue(key, QJsonDocument(entries).toJson(QJsonDocument::Compact));
    settings.sync();
    return settings.status() == QSettings::NoError;
}

QString validate(const QJsonValue &value, const QJsonObject &schema, const QString &path) {
    const auto type = schema["type"].toString();
    if (type == "integer" && (!value.isDouble() || std::floor(value.toDouble()) != value.toDouble()))
        return path + " must be an integer";
    if (type == "number" && !value.isDouble()) return path + " must be a number";
    if (type == "boolean" && !value.isBool()) return path + " must be a boolean";
    if (type == "array" && !value.isArray()) return path + " must be an array";
    if (type == "array" && schema.contains("items")) {
        const auto items = value.toArray();
        for (int index = 0; index < items.size(); ++index) {
            const auto error = validate(items.at(index), schema["items"].toObject(),
                QString("%1[%2]").arg(path).arg(index));
            if (!error.isEmpty()) return error;
        }
    }
    if (type == "string" && !value.isString()) return path + " must be text";
    if (type == "null" && !value.isNull()) return path + " must be null";
    if (value.isDouble() && schema.contains("minimum") && value.toDouble() < schema["minimum"].toDouble())
        return path + " must be at least " + QString::number(schema["minimum"].toDouble());
    if (value.isString() && value.toString().size() < schema["minLength"].toInt())
        return path + " needs at least " + countOf(schema["minLength"].toInt(), "character");
    if (type != "object") return {};
    if (!value.isObject()) return path + " must be an object";
    const auto object = value.toObject();
    for (const auto &key : strings(schema["required"].toArray()))
        if (!object.contains(key)) return path + "." + key + " is required";
    const auto properties = schema["properties"].toObject();
    for (auto it = properties.begin(); it != properties.end(); ++it) {
        if (!object.contains(it.key())) continue;
        const auto error = validate(object[it.key()], it.value().toObject(), path + "." + it.key());
        if (!error.isEmpty()) return error;
    }
    return {};
}

QJsonValue exampleValue(const QString &type, const QJsonObject &sample) {
    const auto lines = strings(sample["lines"].toArray());
    if (type == "text") return lines.isEmpty() ? QString("Example text") : lines.join(' ');
    if (type == "index") return sample["active"];
    if (type == "clause") return lines.value(sample["active"].toInt() - 1);
    if (type == "integer" || type == "number") return 3;
    if (type == "boolean") return true;
    if (type == "object") return QJsonObject{{"name", "Example"}};
    if (type == "array") return QJsonArray{QJsonObject{{"name", "Example"}}};
    if (type == "fields") return sample["fields"];
    if (type == "order") return price(sample["fields"].toObject());
    return QJsonObject{{"decision", "approved"}};
}

QJsonObject sampleFor(const QJsonArray &samples, const QString &input) {
    for (const auto &value : samples) {
        const auto sample = value.toObject();
        if (strings(sample["lines"].toArray()).join(' ') == input) return sample;
    }
    return samples.first().toObject();
}

qint64 stepDelay(const QString &kind) {
    static const QHash<QString, qint64> delays{
        {"input", 280}, {"selector", 910}, {"lookup", 290}, {"extractor", 1130},
        {"pricing", 320}, {"tool", 300},
    };
    return delays.value(kind, 360);
}

void finishStep(QJsonObject &run, QJsonArray steps, QJsonObject step, qint64 now, const QString &status) {
    const int cursor = run["cursor"].toInt();
    step["status"] = status;
    if (status == "passed" || status == "failed") step["duration"] = stepDelay(BlueprintPreview::operationId(step));
    steps.replace(cursor, step);
    if (status == "passed" && cursor + 1 < steps.size()) {
        auto next = steps.at(cursor + 1).toObject();
        next["status"] = "running";
        steps.replace(cursor + 1, next);
        run["cursor"] = cursor + 1;
        run["nextAt"] = now + stepDelay(BlueprintPreview::operationId(next));
        run["status"] = "running";
    } else {
        run["status"] = status;
        run["ended"] = now;
        for (int i = cursor + 1; i < steps.size(); ++i) {
            auto later = steps.at(i).toObject();
            later["status"] = "skipped";
            steps.replace(i, later);
        }
    }
    run["steps"] = steps;
}
}

BlueprintPreview::BlueprintPreview(QObject *parent, bool persist)
    : BlueprintPreview(nullptr, parent, persist) {}

BlueprintPreview::BlueprintPreview(BackendClient &backend, QObject *parent)
    : BlueprintPreview(&backend, parent, true) {}

BlueprintPreview::BlueprintPreview(BackendClient *backend, QObject *parent, bool persist)
    : QObject(parent), m_persist(persist), m_timer(new QTimer(this)), m_backend(backend) {
    QFile file(":/blueprints/order-preview.json");
    if (file.open(QIODevice::ReadOnly)) m_fixtures = QJsonDocument::fromJson(file.readAll()).object();
    QFile catalogFile(":/blueprints/node-catalog.json");
    const auto catalog = catalogFile.open(QIODevice::ReadOnly)
        ? QJsonDocument::fromJson(catalogFile.readAll()).object() : QJsonObject{};
    auto contracts = m_fixtures["contracts"].toObject();
    const auto genericContracts = catalog["contracts"].toObject();
    for (auto it = genericContracts.begin(); it != genericContracts.end(); ++it)
        contracts.insert(it.key(), it.value());
    m_fixtures["contracts"] = contracts;
    m_fixtures["nodeTypes"] = catalog["nodeTypes"];
    m_fixtures["portTypes"] = QJsonArray{"text", "integer", "number", "boolean", "object", "array"};

    auto operations = catalog["operations"].toArray();
    const auto legacy = m_fixtures["kinds"].toObject();
    for (const auto &id : {"selector", "extractor", "lookup", "pricing", "tool"}) {
        const auto base = legacy[id].toObject();
        const QString kind = id == QString("selector") || id == QString("extractor") ? "model"
            : id == QString("tool") ? "tool" : "transform";
        QJsonObject spec{{"id", id}, {"kind", kind}, {"label", base["name"].toString() + " (order example)"},
            {"detail", base["detail"]}, {"legacyOnly", id == QString("tool")}};
        for (const auto &side : {QString("in"), QString("out")}) {
            QJsonArray ports;
            for (const auto &type : strings(base[side].toArray()))
                ports.append(QJsonObject{{"id", type}, {"name", contracts.value(type).toObject()["label"]}, {"type", type}});
            spec[side] = ports;
        }
        operations.append(spec);
    }
    m_fixtures["operations"] = operations;

    if (live()) {
        QJsonArray types;
        for (const auto &value : m_fixtures["nodeTypes"].toArray()) {
            types.append(value);
            if (value.toObject()["id"] != "model") continue;
            types.append(QJsonObject{{"id", "reasoning"}, {"label", "Reasoning"},
                {"detail", "Run a thinking checkpoint."}});
            types.append(QJsonObject{{"id", "adapter"}, {"label", "Adapter"},
                {"detail", "Attach trained LoRA weights."}});
            types.append(QJsonObject{{"id", "knowledge"}, {"label", "Knowledge"},
                {"detail", "Search your local documents."}});
        }
        m_fixtures["nodeTypes"] = types;
        contracts["null"] = QJsonObject{{"label", "Null"}, {"schema", QJsonObject{{"type", "null"}}}};
        contracts["receipt"] = QJsonObject{{"label", "Receipt"}, {"schema", QJsonObject{{"type", "object"}}}};
        contracts["adapter-ref"] = QJsonObject{{"label", "LoRA adapter"}, {"schema", QJsonObject{{"type", "string"}}}};
        m_fixtures["contracts"] = contracts;
        m_fixtures["portTypes"] = QJsonArray{"text", "integer", "number", "boolean", "object", "array", "null"};
        QJsonArray catalog;
        for (const auto &value : operations) {
            auto spec = value.toObject();
            const auto op = spec["id"].toString();
            if (op == "selector" || op == "extractor" || op == "tool" || op == "lookup" || op == "pricing") continue;
            if (op == "input") {
                auto port = spec["out"].toArray().first().toObject();
                port["id"] = "value";
                spec["out"] = QJsonArray{port};
            } else if (op == "approval") {
                spec["label"] = "Request approval";
                spec["detail"] = "Request approval for an action supplied as a structured object.";
                spec["in"] = QJsonArray{QJsonObject{{"id", "value"}, {"name", "Action"}, {"type", "object"}}};
            }
            catalog.append(spec);
        }
        const QJsonArray modelIn{QJsonObject{{"id", "value"}, {"name", "Input"}, {"type", "text"}, {"chooseType", true}}};
        const QJsonArray modelOut{QJsonObject{{"id", "value"}, {"name", "Result"}, {"type", "text"}, {"chooseType", true}}};
        const QJsonObject adapterPort{{"id", "adapter"}, {"name", "LoRA adapter"}, {"type", "adapter-ref"}};
        auto modelPorts = modelIn;
        modelPorts.append(adapterPort);
        catalog.append(QJsonObject{{"id", "generate"}, {"kind", "model"}, {"label", "Generate a response"},
            {"detail", "Send named inputs to a local Transformers checkpoint. Return typed JSON or a single plain-text result."},
            {"in", modelPorts}, {"out", modelOut}});
        catalog.append(QJsonObject{{"id", "reason"}, {"kind", "reasoning"}, {"label", "Reason through a task"},
            {"detail", "Use a Transformers checkpoint with explicit thinking. The output contains the final answer."},
            {"in", modelPorts}, {"out", modelOut}});
        catalog.append(QJsonObject{{"id", "lora"}, {"kind", "adapter"}, {"label", "Attach trained LoRA"},
            {"detail", "Select local adapter weights and connect them to the LoRA adapter port of a compatible checkpoint."},
            {"out", QJsonArray{adapterPort}}});
        // Retired: kept so older graphs still draw and show the repair message.
        catalog.append(QJsonObject{{"id", "pi-reply"}, {"kind", "agent"}, {"label", "Retired Pi Agent"},
            {"detail", "This node was removed. Replace it with a Model node that has Plain text responses and put its instructions in the system prompt."},
            {"legacyOnly", true},
            {"in", QJsonArray{QJsonObject{{"id", "task"}, {"name", "Task"}, {"type", "text"}}}},
            {"out", QJsonArray{QJsonObject{{"id", "reply"}, {"name", "Reply"}, {"type", "text"}}}},
            {"setting", QJsonObject{{"label", "Agent instructions"}, {"multiline", true},
                                    {"placeholder", "Summarize the request in three sentences."}}}});
        catalog.append(QJsonObject{{"id", "agent"}, {"kind", "tool"}, {"label", "Ask the local tool model"},
            {"detail", "Use the loaded tool model. Each external action requires approval."}, {"in", modelIn},
            {"legacyOnly", true},
            {"out", QJsonArray{QJsonObject{{"id", "receipt"}, {"name", "Result"}, {"type", "receipt"}}}}});
        catalog.append(QJsonObject{{"id", "constant"}, {"kind", "transform"}, {"label", "Provide a constant"},
            {"detail", "Supply a fixed JSON value such as a label, threshold, or default setting."},
            {"setting", QJsonObject{{"label", "Value (JSON)"}, {"default", "\"Example\""}, {"multiline", true}}},
            {"out", QJsonArray{QJsonObject{{"id", "value"}, {"name", "Value"}, {"type", "text"}, {"chooseType", true}}}}});
        catalog.append(QJsonObject{{"id", "build-object"}, {"kind", "transform"}, {"label", "Build an object"},
            {"detail", "Combine connected values into one JSON object. Input keys become its field names."},
            {"in", QJsonArray{QJsonObject{{"id", "field"}, {"name", "Field"}, {"type", "text"}, {"chooseType", true}}}},
            {"out", QJsonArray{QJsonObject{{"id", "value"}, {"name", "Object"}, {"type", "object"}}}}});
        const auto port = [](const char *id, const char *name, const char *type, bool choose = false) {
            QJsonObject entry{{"id", id}, {"name", name}, {"type", type}};
            if (choose) entry["chooseType"] = true;
            return entry;
        };
        catalog.append(QJsonObject{{"id", "template"}, {"kind", "transform"}, {"label", "Fill a text template"},
            {"detail", "Insert named inputs into saved text. Write {key} for an input port; use {{ and }} for literal braces."},
            {"setting", QJsonObject{{"label", "Template"}, {"multiline", true}, {"default", "Summary for {name}"},
                                    {"placeholder", "Dear {name}, your order {order} is {status}."}}},
            {"in", QJsonArray{port("name", "Name", "text", true)}},
            {"out", QJsonArray{port("value", "Text", "text")}}});
        QJsonArray comparisons;
        for (const auto &[id, label] : {std::pair{"equals", "Equals"}, {"not-equals", "Does not equal"},
                 {"greater", "Is greater than"}, {"greater-or-equal", "Is at least"}, {"less", "Is less than"},
                 {"less-or-equal", "Is at most"}, {"contains", "Contains"}})
            comparisons.append(QJsonArray{id, label});
        catalog.append(QJsonObject{{"id", "compare"}, {"kind", "transform"}, {"label", "Compare two values"},
            {"detail", "Return true or false. Ordering compares numbers; Contains looks inside text or an array."},
            {"setting", QJsonObject{{"label", "Comparison"}, {"options", comparisons}, {"default", "equals"}}},
            {"in", QJsonArray{port("left", "Value", "number", true), port("right", "Compared with", "number", true)}},
            {"out", QJsonArray{port("value", "Result", "boolean")}}});
        catalog.append(QJsonObject{{"id", "choose"}, {"kind", "transform"}, {"label", "Choose a value"},
            {"detail", "Return one of two connected values based on a true or false condition. Both values are computed."},
            {"in", QJsonArray{port("condition", "Condition", "boolean"), port("if_true", "If true", "text", true),
                              port("if_false", "If false", "text", true)}},
            {"out", QJsonArray{port("value", "Chosen value", "text", true)}}});
        catalog.append(QJsonObject{{"id", "validate"}, {"kind", "transform"}, {"label", "Validate data"},
            {"detail", "Check a value against saved rules. A mismatch fails this step with the field path, and later steps are skipped."},
            {"setting", QJsonObject{{"label", "Rules (JSON Schema subset)"}, {"multiline", true},
                {"default", "{\n  \"type\": \"object\",\n  \"properties\": {\"id\": {\"type\": \"string\", \"minLength\": 1}},\n  \"required\": [\"id\"]\n}"}}},
            {"in", QJsonArray{port("value", "Value", "object")}},
            {"out", QJsonArray{port("value", "Valid value", "object")}}});
        catalog.append(QJsonObject{{"id", "filter"}, {"kind", "transform"}, {"label", "Filter rows"},
            {"detail", "Keep the rows whose field passes a rule. Rows without the field are left out."},
            {"setting", QJsonObject{{"label", "Rule"}, {"default", "amount >= 100"},
                                    {"placeholder", "status = \"open\"   ·   tags contains \"urgent\""}}},
            {"sameType", true},
            {"in", QJsonArray{port("rows", "Rows", "array")}}, {"out", QJsonArray{port("rows", "Matching rows", "array")}}});
        catalog.append(QJsonObject{{"id", "sort"}, {"kind", "transform"}, {"label", "Sort rows"},
            {"detail", "Order rows by one field. Add desc for largest first. Rows without the field go last."},
            {"setting", QJsonObject{{"label", "Sort by"}, {"default", "amount desc"}, {"placeholder", "name   ·   amount desc"}}},
            {"sameType", true},
            {"in", QJsonArray{port("rows", "Rows", "array")}}, {"out", QJsonArray{port("rows", "Sorted rows", "array")}}});
        catalog.append(QJsonObject{{"id", "search"}, {"kind", "knowledge"}, {"label", "Search documents"},
            {"detail", "Find passages in the Knowledge panel's documents. Connect Context to a Model prompt, or use Results for scores and sources."},
            {"setting", QJsonObject{{"label", "Number of passages"}, {"placeholder", "Blank uses the Knowledge panel setting"}}},
            {"in", QJsonArray{port("query", "Query", "text")}},
            {"out", QJsonArray{port("results", "Results", "array"), port("context", "Context", "text")}}});
        for (const auto &op : {QString("order-clause"), QString("order-price")})
            catalog.append(QJsonObject{{"id", op}, {"kind", "transform"}, {"label", op == "order-clause"
                ? "Look up order clause" : "Validate and price order"}, {"legacyOnly", true}});
        m_fixtures["operations"] = catalog;
        for (const auto &key : {"models", "adapters", "samples", "scenarios", "baseline", "starter"})
            m_fixtures.remove(key);
        const QSettings settings;
        const auto home = qEnvironmentVariable("MAGNITE_HOME", settings.value("storage/dataDir").toString().trimmed());
        const auto identity = home.isEmpty() ? QByteArray{} : QFileInfo(home).absoluteFilePath().toUtf8();
        const auto profile = QCryptographicHash::hash(identity, QCryptographicHash::Sha256).toHex();
        m_draftsKey = "blueprints/" + profile + "/drafts";
        m_templatesKey = "blueprints/" + profile + "/nodes";
        m_saved = QJsonDocument::fromJson(settings.value(QString::fromLatin1(m_draftsKey)).toByteArray()).array();
        m_nodeLibrary = QJsonDocument::fromJson(settings.value(QString::fromLatin1(m_templatesKey)).toByteArray()).array();
        const auto connectionChanged = [this] {
            ++m_session;
            m_streams.clear();
            const auto waiting = std::exchange(m_refreshingRuns, {});
            for (const auto &waiters : waiting)
                for (const auto &waiter : waiters) waiter({}, "The local service connection changed.");
            m_dirtyRuns.clear();
            m_serviceError = connected() ? QString{} : QString("The local service is disconnected. Reconnecting…");
            emit serviceChanged();
            if (!connected()) return;
            refreshSaved();
            refreshRuns("?limit=50");
            refreshRuns("?active=true");
            refreshRuns("?surface=chat&limit=20", true);
            refreshOptions();
        };
        connect(backend, &BackendClient::baseUrlChanged, this, connectionChanged);
        connectionChanged();
        connect(m_timer, &QTimer::timeout, this, [this] {
            if (!connected()) return;
            const auto records = m_runs;
            for (const auto &value : records) {
                const auto record = value.toObject();
                const auto id = record["id"].toString();
                const auto state = record["status"].toString();
                if ((state == "queued" || state == "running" || state == "waiting") &&
                    !m_streams.contains(id) && !m_refreshingRuns.contains(id))
                    loadRun(id);
            }
        });
        m_timer->start(2500);
        return;
    }

    if (m_persist) {
        const QSettings settings;
        m_nodeLibrary = QJsonDocument::fromJson(settings.value(nodeLibraryKey).toByteArray()).array();
        m_saved = QJsonDocument::fromJson(settings.value(savedKey).toByteArray()).array();
        m_runs = QJsonDocument::fromJson(settings.value(runsKey).toByteArray()).array();
    }
    const auto starter = m_fixtures["starter"].toObject();
    if (!starter.isEmpty() && runs(starter["id"].toString()).isEmpty()) {
        // Seed the same completed and failed traces as the browser preview.
        QString error;
        for (const auto &scenario : {QString("failure"), QString("normal")}) {
            const auto sample = m_fixtures["samples"].toArray().at(scenario == "normal" ? 1 : 0).toObject();
            const auto runId = start(starter, strings(sample["lines"].toArray()).join(' '), {}, scenario, "Blueprints", &error);
            if (runId.isEmpty()) continue;
            auto run = m_runs.first().toObject();
            const qint64 stamp = QDateTime::currentMSecsSinceEpoch() - (scenario == "normal" ? 3600000 : 7200000);
            run["created"] = stamp;
            run["nextAt"] = stamp + stepDelay("input");
            while (run["status"].toString() == "running") advance(run, run["nextAt"].toInteger());
            m_runs.replace(0, run);
        }
        storeRuns();
    }
    connect(m_timer, &QTimer::timeout, this, &BlueprintPreview::tick);
    m_timer->start(200);
}

QJsonObject BlueprintPreview::example() const {
    const auto savedExample = findById(m_saved, "order-intake");
    return savedExample.isEmpty() ? m_fixtures["starter"].toObject() : savedExample;
}

QJsonObject BlueprintPreview::blueprint(const QString &id) const {
    return findById(m_saved, id);
}

bool BlueprintPreview::saveBlueprint(const QJsonObject &blueprint) {
    auto saved = m_saved;
    const auto key = blueprint["id"].toString();
    int index = 0;
    while (index < saved.size() && saved.at(index).toObject()["id"].toString() != key) ++index;
    if (index == saved.size()) saved.prepend(blueprint);
    else saved.replace(index, blueprint);
    return storeBlueprints(saved);
}

bool BlueprintPreview::deleteBlueprint(const QString &id) {
    if (id.isEmpty()) return false;
    auto saved = m_saved;
    for (int index = 0; index < saved.size(); ++index) {
        if (saved.at(index).toObject()["id"].toString() != id) continue;
        saved.removeAt(index);
        return storeBlueprints(saved);
    }
    return false;
}

QJsonArray BlueprintPreview::compatibleAdapters(const QString &model, const QString &kind) const {
    QJsonArray result;
    for (const auto &value : m_fixtures["adapters"].toArray()) {
        const auto adapter = value.toObject();
        if (adapter["model"].toString() == model && adapter["kind"].toString() == kind) result.append(adapter);
    }
    return result;
}

bool BlueprintPreview::storeBlueprints(const QJsonArray &saved) {
    if (!writeEntries(live() ? m_draftsKey.constData() : savedKey, saved, m_persist)) return false;
    m_saved = saved;
    emit savedChanged();
    return true;
}

bool BlueprintPreview::storeNodeLibrary(const QJsonArray &nodes) {
    if (!writeEntries(live() ? m_templatesKey.constData() : nodeLibraryKey, nodes, m_persist)) return false;
    m_nodeLibrary = nodes;
    emit nodeLibraryChanged();
    return true;
}

bool BlueprintPreview::saveNodeTemplate(const QString &name, QJsonObject node) {
    if (name.trimmed().isEmpty() || operation(operationId(node)).isEmpty()) return false;
    node.remove("id");
    node.remove("x");
    node.remove("y");
    auto entries = m_nodeLibrary;
    entries.prepend(QJsonObject{{"id", id()}, {"name", name.trimmed()}, {"node", node}});
    return storeNodeLibrary(entries);
}

bool BlueprintPreview::removeNodeTemplate(const QString &entryId) {
    auto entries = m_nodeLibrary;
    for (int index = 0; index < entries.size(); ++index) {
        if (entries.at(index).toObject()["id"].toString() != entryId) continue;
        entries.removeAt(index);
        return storeNodeLibrary(entries);
    }
    return false;
}

QString BlueprintPreview::operationId(const QJsonObject &node) {
    return node["operation"].toString(node["kind"].toString());
}

QJsonObject BlueprintPreview::operation(const QString &id) const {
    return findById(m_fixtures["operations"].toArray(), id);
}

QString BlueprintPreview::settingError(const QJsonObject &node) const {
    const auto code = operationId(node);
    const auto value = node["setting"].toString();
    if (code == "dataset-rows" && node["dataset"].toString().isEmpty() && !parseJsonValue(value).isArray())
        return "Enter a valid JSON array for the dataset rows.";
    if (code == "field" && value.trimmed().isEmpty()) return "Enter the name of the field to read.";
    if (code == "constant") {
        const auto parsed = parseJsonValue(value);
        if (parsed.isUndefined()) return "Enter a valid JSON value. Use double quotes around text.";
        const auto error = validate(parsed, node["out"].toArray().first().toObject()["schema"].toObject(), "Value");
        if (!error.isEmpty()) return error;
    }
    if (code == "pi-reply" && value.size() > 16000) return "Use at most 16,000 characters for agent instructions.";
    if (code == "template") {
        if (value.trimmed().isEmpty()) return "Enter the template text.";
        QSet<QString> keys;
        for (const auto &port : node["in"].toArray()) keys.insert(port.toObject()["id"].toString());
        static const QRegularExpression field(R"(\{\{|\}\}|\{([A-Za-z][A-Za-z0-9_-]{0,63})\})");
        for (auto it = field.globalMatch(value); it.hasNext();) {
            const auto name = it.next().captured(1);
            if (!name.isEmpty() && !keys.contains(name))
                return QString("Add an input port with the key %1, or remove {%1} from the template.").arg(name);
        }
    }
    if (code == "filter" && parseRule(value).isEmpty())
        return "Write the rule as field, comparison, value. Comparisons: = != > >= < <= contains.";
    if (code == "sort" && parseSort(value).isEmpty()) return "Enter a field name, optionally followed by desc.";
    if (code == "search" && !value.trimmed().isEmpty()) {
        bool number = false;
        const int count = value.trimmed().toInt(&number);
        if (!number || count < 1 || count > 50) return "Enter a number of passages from 1 to 50, or leave it blank.";
    }
    if (code == "validate") {
        const auto rules = parseJsonValue(value);
        if (!rules.isObject() || !rules.toObject()["type"].isString())
            return "Enter a JSON object with a \"type\", for example {\"type\": \"object\"}.";
    }
    return {};
}

namespace {
const std::pair<const char *, const char *> kRuleOperators[] = {
    {">=", "greater-or-equal"}, {"<=", "less-or-equal"}, {"!=", "not-equals"}, {"=", "equals"},
    {">", "greater"}, {"<", "less"}, {"contains", "contains"}};
}

QJsonObject BlueprintPreview::parseRule(const QString &text) {
    static const QRegularExpression rule(R"(^\s*([A-Za-z_][A-Za-z0-9_.-]*)\s*(>=|<=|!=|=|>|<|\bcontains\b)\s*(.+?)\s*$)");
    const auto match = rule.match(text);
    if (!match.hasMatch()) return {};
    QString op;
    for (const auto &[symbol, name] : kRuleOperators)
        if (match.captured(2) == QLatin1String(symbol)) op = name;
    // A bare word is text; anything that parses as JSON keeps its type.
    auto value = parseJsonValue(match.captured(3));
    if (value.isUndefined()) value = match.captured(3);
    return {{"field", match.captured(1)}, {"operator", op}, {"value", value}};
}

QString BlueprintPreview::ruleText(const QJsonObject &rule) {
    QString symbol = rule["operator"].toString();
    for (const auto &[text, name] : kRuleOperators)
        if (symbol == QLatin1String(name)) symbol = text;
    return rule["field"].toString() + " " + symbol + " " + jsonText(rule["value"]);
}

QJsonObject BlueprintPreview::parseSort(const QString &text) {
    static const QRegularExpression sort(R"(^\s*([A-Za-z_][A-Za-z0-9_.-]*)(?:\s+(asc|ascending|desc|descending))?\s*$)",
                                         QRegularExpression::CaseInsensitiveOption);
    const auto match = sort.match(text);
    if (!match.hasMatch()) return {};
    return {{"field", match.captured(1)}, {"descending", match.captured(2).startsWith("desc", Qt::CaseInsensitive)}};
}

QString BlueprintPreview::nodeKindLabel(const QString &id) const {
    if (id == "agent") return "Retired Pi Agent";
    return findById(m_fixtures["nodeTypes"].toArray(), id)["label"].toString(id);
}

QString BlueprintPreview::portLabel(const QJsonObject &port) const {
    const auto type = QString(port["type"].toString());
    return port["name"].toString(m_fixtures["contracts"].toObject().value(type).toObject()["label"].toString(type));
}

bool BlueprintPreview::usesOrderSample(const QJsonObject &blueprint, const QString &target) const {
    if (target.isEmpty() && blueprint["id"].toString() == "order-intake") return true;
    for (const auto &value : blueprint["nodes"].toArray()) {
        const auto node = value.toObject();
        if (!target.isEmpty() && node["id"].toString() != target) continue;
        const auto code = operationId(node);
        if (code == "selector" || code == "extractor" || code == "lookup" || code == "pricing") return true;
    }
    return false;
}

QJsonObject BlueprintPreview::makeNode(const QString &operationKey, const QString &nodeId, int x, int y) const {
    const auto spec = operation(operationKey);
    if (spec.isEmpty()) return {};
    const auto kind = spec["kind"].toString();
    const auto contracts = m_fixtures["contracts"].toObject();
    QJsonObject node{{"id", nodeId}, {"kind", kind}, {"operation", operationKey},
        // Transform and Knowledge cards cover many operations, so name the node after what it does.
        {"name", kind == "transform" || kind == "knowledge" ? spec["label"].toString() : nodeKindLabel(kind)},
        {"x", x}, {"y", y}};
    if (!live() && kind == "model") node["model"] = "qwen";
    const auto setting = spec["setting"].toObject();
    if (!setting.isEmpty()) node["setting"] = setting["default"].toString();
    for (const auto &side : {QString("in"), QString("out")}) {
        QJsonArray ports;
        for (const auto &value : spec[side].toArray()) {
            auto port = value.toObject();
            const auto type = port["type"].toString();
            port.remove("chooseType");
            port["schema"] = contracts.value(type).toObject()["schema"];
            if (live() && kind == "dataset")
                port["schema"] = QJsonObject{{"type", "array"}, {"items", QJsonObject{{"type", "object"}}}};
            ports.append(port);
        }
        node[side] = ports;
    }
    return node;
}

QJsonObject BlueprintPreview::adapterSource(const QJsonObject &graph, const QJsonObject &node) {
    if (findById(node["in"].toArray(), "adapter")["type"] != "adapter-ref") return {};
    for (const auto &value : graph["edges"].toArray()) {
        const auto edge = value.toObject();
        if (edge["to"] == node["id"] && edge["in"] == "adapter")
            return findById(graph["nodes"].toArray(), edge["from"].toString());
    }
    return {};
}


QString BlueprintPreview::id() { return QUuid::createUuid().toString(QUuid::Id128); }

bool BlueprintPreview::choosesType(const QJsonObject &node, const QString &side, const QString &portId) const {
    return findById(operation(operationId(node))[side].toArray(), portId)["chooseType"].toBool();
}

QJsonObject BlueprintPreview::withPortType(const QJsonObject &node, const QString &side, const QString &portId,
                                           const QString &type) const {
    const QJsonValue schema = m_fixtures["contracts"].toObject().value(type).toObject()["schema"];
    auto result = node;
    auto ports = node[side].toArray();
    for (int i = 0; i < ports.size(); ++i) {
        auto port = ports.at(i).toObject();
        if (port["id"].toString() != portId) continue;
        port["type"] = type;
        port["schema"] = schema;
        ports.replace(i, port);
    }
    result[side] = ports;
    return result;
}

QJsonObject BlueprintPreview::connectGraph(const QJsonObject &blueprint, const QJsonObject &edge, int *droppedEdges,
                                           QString *error) const {
    if (droppedEdges) *droppedEdges = 0;
    if (error) error->clear();
    const auto from = edge["from"].toString(), to = edge["to"].toString();
    const auto out = edge["out"].toString(), in = edge["in"].toString();
    QJsonObject graph = blueprint;
    QJsonArray edges;
    for (const auto &value : blueprint["edges"].toArray()) {
        const auto existing = value.toObject();
        if (existing["to"].toString() == to && existing["in"].toString() == in) continue;  // replaced
        edges.append(existing);
    }
    auto nodes = blueprint["nodes"].toArray();
    const auto indexOf = [&](const QString &id) {
        for (int i = 0; i < nodes.size(); ++i)
            if (nodes.at(i).toObject()["id"].toString() == id) return i;
        return -1;
    };
    const int source = indexOf(from), target = indexOf(to);
    if (source >= 0 && target >= 0) {
        const auto sourceNode = nodes.at(source).toObject(), targetNode = nodes.at(target).toObject();
        const auto sourceType = findById(sourceNode["out"].toArray(), out)["type"].toString();
        const auto targetType = findById(targetNode["in"].toArray(), in)["type"].toString();
        const auto contracts = m_fixtures["contracts"].toObject();
        bool sourceWired = false;
        for (const auto &value : edges)
            sourceWired |= value.toObject()["from"].toString() == from && value.toObject()["out"].toString() == out;
        QString retyped;
        if (sourceType != targetType && !sourceType.isEmpty() && !targetType.isEmpty()) {
            if (choosesType(targetNode, "in", in) && contracts.contains(sourceType)) {
                nodes.replace(target, withPortType(targetNode, "in", in, sourceType));
                retyped = to;
            } else if (choosesType(sourceNode, "out", out) && !sourceWired && contracts.contains(targetType)) {
                // An output that already feeds other inputs keeps its type; only a free one adopts the target's.
                nodes.replace(source, withPortType(sourceNode, "out", out, targetType));
                retyped = from;
            }
        }
        graph["nodes"] = nodes;
        if (!retyped.isEmpty()) {
            for (int i = edges.size() - 1; i >= 0; --i) {
                const auto other = edges.at(i).toObject();
                if (other["from"].toString() != retyped && other["to"].toString() != retyped) continue;
                auto without = edges;
                without.removeAt(i);
                auto probe = graph;
                probe["edges"] = without;
                if (connectionError(probe, other).isEmpty()) continue;
                edges = without;
                if (droppedEdges) ++*droppedEdges;
            }
        }
    }
    graph["edges"] = edges;
    const auto problem = connectionError(graph, edge);
    if (!problem.isEmpty()) {
        if (error) *error = problem;
        return {};
    }
    edges.append(edge);
    graph["edges"] = edges;
    return graph;
}

QString BlueprintPreview::dropError(const QJsonObject &blueprint, const QJsonObject &edge) const {
    QString error;
    connectGraph(blueprint, edge, nullptr, &error);
    return error;
}

QString BlueprintPreview::connectionError(const QJsonObject &blueprint, const QJsonObject &edge) const {
    const auto from = edge["from"].toString();
    const auto to = edge["to"].toString();
    const auto nodes = blueprint["nodes"].toArray();
    const auto outgoing = findById(findById(nodes, from)["out"].toArray(), edge["out"].toString());
    const auto incoming = findById(findById(nodes, to)["in"].toArray(), edge["in"].toString());
    if (outgoing.isEmpty() || incoming.isEmpty()) return "Select an output port and an input port.";
    const auto fromType = outgoing["schema"].toObject()["type"].toString();
    const auto toType = incoming["schema"].toObject()["type"].toString();
    const bool adapterOutput = outgoing["type"] == "adapter-ref";
    const bool adapterInput = incoming["type"] == "adapter-ref";
    if (adapterOutput != adapterInput) return "Connect LoRA adapter outputs to LoRA adapter inputs.";
    if (adapterInput && !findById(nodes, to)["adapter"].toString().isEmpty())
        return "Clear the checkpoint's direct adapter binding before connecting a LoRA adapter.";
    if (fromType != toType && !(live() && fromType == "integer" && toType == "number")) {
        const auto contracts = m_fixtures["contracts"].toObject();
        return QString("%1 cannot connect to %2. Choose an input with the same type.")
            .arg(contracts.value(outgoing["type"].toString()).toObject()["label"].toString(),
                 contracts.value(incoming["type"].toString()).toObject()["label"].toString());
    }
    const auto edges = blueprint["edges"].toArray();
    for (const auto &value : edges) {
        const auto existing = value.toObject();
        if (existing["to"] == edge["to"] && existing["in"] == edge["in"])
            return "This input already has a connection. Remove it before connecting a new output.";
    }
    QStringList pending{to};
    QSet<QString> visited;
    while (!pending.isEmpty()) {
        const auto next = pending.takeLast();
        if (next == from) return "This connection would create a cycle.";
        if (visited.contains(next)) continue;
        visited.insert(next);
        for (const auto &value : edges) {
            const auto existing = value.toObject();
            if (existing["from"].toString() == next) pending.append(existing["to"].toString());
        }
    }
    return {};
}

QStringList BlueprintPreview::issues(const QJsonObject &blueprint) const {
    QStringList result;
    bool hasSource = false;
    const auto nodes = blueprint["nodes"].toArray();
    const auto edges = blueprint["edges"].toArray();
    for (const auto &value : nodes) {
        const auto node = value.toObject();
        const auto name = node["name"].toString();
        const auto kind = operationId(node);
        hasSource |= kind == "input" || kind == "dataset-rows" || kind == "constant";
        const auto invalidSetting = settingError(node);
        if (!invalidSetting.isEmpty()) result << name + " · " + invalidSetting;
        if (live() && (kind == "generate" || kind == "reason") && node["model"].toString().isEmpty())
            result << name + " needs a local checkpoint.";
        if (kind == "lora" && node["adapter"].toString().isEmpty())
            result << name + " needs a trained adapter.";
        if (kind == "selector" || kind == "extractor") {
            const auto model = findById(m_fixtures["models"].toArray(), node["model"].toString());
            const auto adapter = findById(compatibleAdapters(node["model"].toString(), kind), node["adapter"].toString());
            if (!model["supported"].toBool()) result << name + " needs a Torch / PEFT checkpoint.";
            else if (adapter.isEmpty()) result << name + " needs a compatible adapter.";
        }
        for (const auto &portValue : node["in"].toArray()) {
            const auto port = portValue.toObject();
            if (port["type"] == "adapter-ref") continue;
            const bool connected = std::any_of(edges.cbegin(), edges.cend(), [&](const auto &edgeValue) {
                const auto edge = edgeValue.toObject();
                return edge["to"] == node["id"] && edge["in"] == port["id"];
            });
            if (!connected)
                result << QString("%1 needs a connection to %2.").arg(name, portLabel(port));
        }
    }
    if (!hasSource) result.prepend(live() ? "Add an Input, Dataset, or Constant node." : "Add an Input or Dataset node.");
    if (live() && std::none_of(nodes.begin(), nodes.end(), [](const auto &node) {
        return node.toObject()["kind"].toString() == "output";
    })) result << "Add an Output node.";
    for (int i = 0; i < edges.size(); ++i) {
        auto remaining = edges;
        remaining.removeAt(i);
        auto graph = blueprint;
        graph["edges"] = remaining;
        const auto error = connectionError(graph, edges.at(i).toObject());
        if (!error.isEmpty()) result.append(error);
    }
    result.removeDuplicates();
    return result;
}

QString BlueprintPreview::start(const QJsonObject &blueprint, const QString &input, const QString &target,
                                const QString &scenario, const QString &source, QString *error) {
    auto fail = [error](const QString &message) {
        if (error) *error = message;
        return QString{};
    };
    const bool orderSample = usesOrderSample(blueprint, target);
    if ((blueprint.isEmpty() || orderSample) && input.trimmed().isEmpty()) return fail("Enter a sample message.");
    const QString runInput = orderSample ? input.trimmed() : input;
    const auto edges = blueprint["edges"].toArray();
    QJsonArray steps;
    if (blueprint.isEmpty()) {
        steps.append(QJsonObject{{"id", "chat-tool"}, {"kind", "chat-tool"}, {"name", "Review tool request"}});
    } else if (!target.isEmpty()) {
        auto node = findById(blueprint["nodes"].toArray(), target);
        if (node.isEmpty()) return fail("Select an existing node to test.");
        steps.append(node);
    } else {
        const auto problems = issues(blueprint);
        if (!problems.isEmpty()) return fail(problems.join('\n'));
        auto remaining = blueprint["nodes"].toArray();
        QSet<QString> ordered;
        bool hasTool = false;
        while (!remaining.isEmpty()) {
            int index = 0;
            for (; index < remaining.size(); ++index) {
                const auto node = remaining.at(index).toObject();
                if (std::none_of(edges.cbegin(), edges.cend(), [&](const auto &value) {
                    const auto edge = value.toObject();
                    return edge["to"] == node["id"] && !ordered.contains(edge["from"].toString());
                })) break;
            }
            if (index == remaining.size()) return fail("This workflow contains a cycle.");
            const auto node = remaining.takeAt(index).toObject();
            ordered.insert(node["id"].toString());
            hasTool |= operationId(node) == "tool" || operationId(node) == "approval";
            steps.append(node);
        }
        if (!hasTool && (scenario == "approval" || scenario == "command"))
            steps.append(QJsonObject{{"id", "preview-tool"}, {"kind", "tool"},
                                     {"name", scenario == "command" ? "Check Python version" : "Open order preview"}});
    }
    for (int i = 0; i < steps.size(); ++i) {
        auto step = steps.at(i).toObject();
        step["status"] = i == 0 ? "running" : "queued";
        steps.replace(i, step);
    }
    const auto runId = id();
    const auto now = QDateTime::currentMSecsSinceEpoch();
    const auto sample = orderSample ? sampleFor(m_fixtures["samples"].toArray(), runInput.trimmed()) : QJsonObject{};
    const QJsonObject run{{"id", runId}, {"blueprintId", blueprint["id"].toString()},
        {"name", blueprint.isEmpty() ? QString("Chat tool request") : blueprint["name"].toString()},
        {"revision", blueprint["revision"].toInt()}, {"input", runInput}, {"target", target}, {"scenario", scenario},
        {"source", source}, {"toolChat", blueprint.isEmpty()},
        {"sampleId", sample["id"]}, {"status", "running"}, {"created", now}, {"cursor", 0},
        {"nextAt", now + stepDelay(operationId(steps.first().toObject()))}, {"steps", steps}, {"edges", edges}};
    m_runs.prepend(run);
    storeRuns();
    emit runChanged(runId);
    return runId;
}

bool BlueprintPreview::advance(QJsonObject &run, qint64 now) const {
    const auto state = run["status"].toString();
    if (state != "running" && state != "waiting") return false;
    auto steps = run["steps"].toArray();
    const int cursor = run["cursor"].toInt(-1);
    if (cursor < 0 || cursor >= steps.size()) return false;
    auto step = steps.at(cursor).toObject();
    if (state == "waiting") {
        auto decision = run["decision"].toObject();
        const auto expires = decision["expires"].toInteger();
        if (now < expires) return false;
        decision["status"] = "expired";
        run["decision"] = decision;
        finishStep(run, steps, step, expires, "expired");
        return true;
    }
    const auto eventAt = run["nextAt"].toInteger();
    if (now < eventAt) return false;

    const auto kind = operationId(step);
    const auto sample = findById(m_fixtures["samples"].toArray(), run["sampleId"].toString());
    const auto rawInput = run["input"].toString();
    const auto ports = step["in"].toArray();
    QJsonObject inputs, namedInputs;
    const bool singleNode = !run["target"].toString().isEmpty();
    const bool orderOperation = kind == "selector" || kind == "extractor" || kind == "lookup" || kind == "pricing";
    for (const auto &value : ports) {
        const auto port = value.toObject();
        const auto inputId = port["id"].toString();
        QJsonValue input = QJsonValue::Undefined;
        if (singleNode) {
            const auto type = port["schema"].toObject()["type"].toString();
            if (!orderOperation && ports.size() == 1 && !rawInput.trimmed().isEmpty())
                input = type == "string" ? QJsonValue(rawInput) : parseJsonValue(rawInput);
            else input = exampleValue(port["type"].toString(), sample);
        } else {
            for (const auto &edgeValue : run["edges"].toArray()) {
                const auto edge = edgeValue.toObject();
                if (edge["to"] == step["id"] && edge["in"] == port["id"])
                    input = findById(steps, edge["from"].toString())["output"];
            }
        }
        inputs.insert(inputId, input);
        namedInputs.insert(portLabel(port), input);
    }
    if (kind == "input") namedInputs["Run input"] = rawInput;
    if (kind == "dataset-rows") namedInputs["Dataset rows"] = parseJsonValue(step["setting"].toString());
    step["inputs"] = namedInputs;

    QString error = settingError(step);
    if (kind == "selector" || kind == "extractor") {
        const auto model = findById(m_fixtures["models"].toArray(), step["model"].toString());
        const auto adapter = findById(compatibleAdapters(step["model"].toString(), kind), step["adapter"].toString());
        if (!model["supported"].toBool() || adapter.isEmpty())
            error = "Choose a compatible Torch / PEFT model and adapter before testing this node.";
    }
    for (const auto &value : ports) {
        if (!error.isEmpty()) break;
        const auto port = value.toObject();
        error = validate(inputs.value(port["id"].toString()), port["schema"].toObject(), "input." + portLabel(port));
    }
    if (error.isEmpty() && (kind == "tool" || kind == "chat-tool" || kind == "approval")) {
        const auto requests = m_fixtures["approvalFixtures"].toObject();
        auto decision = requests[run["scenario"].toString() == "command" ? "command" : "file"].toObject();
        decision["status"] = "waiting";
        decision["stepId"] = step["id"];
        decision["expires"] = eventAt + approvalMs;
        run["decision"] = decision;
        run["status"] = "waiting";
        step["status"] = "waiting";
        steps.replace(run["cursor"].toInt(), step);
        run["steps"] = steps;
        return true;
    }
    if (error.isEmpty()) {
        QJsonValue output;
        const auto outputPorts = step["out"].toArray();
        const auto value = inputs.value("value");
        if (kind == "input") {
            const auto schema = outputPorts.at(0).toObject()["schema"].toObject();
            output = schema["type"].toString() == "string" ? QJsonValue(rawInput) : parseJsonValue(rawInput);
        } else if (kind == "dataset-rows") output = parseJsonValue(step["setting"].toString());
        else if (kind == "pass" || kind == "output") output = value;
        else if (kind == "parse-json") {
            output = parseJsonValue(value.toString());
            if (output.isUndefined()) error = "The input is not valid JSON.";
        } else if (kind == "field") {
            const auto property = step["setting"].toString();
            output = value.toObject().value(property);
            if (output.isUndefined()) error = QString("The object has no field named “%1”.").arg(property);
        } else if (kind == "first") {
            if (value.toArray().isEmpty()) error = "The array has no items.";
            else output = value.toArray().first();
        } else if (kind == "count") output = int(value.toArray().size());
        else if (kind == "format") output = value.isString() ? value : QJsonValue(jsonText(value));
        else if (kind == "selector") output = sample["active"];
        else if (kind == "lookup") {
            static const QRegularExpression clausePattern("[^.!?]+[.!?]*");
            QStringList clauses;
            auto matches = clausePattern.globalMatch(inputs["text"].toString());
            while (matches.hasNext()) clauses.append(matches.next().captured().trimmed());
            const int index = inputs["index"].toInt() - 1;
            if (index < 0 || index >= clauses.size()) error = "The selected clause index is outside the message.";
            else output = clauses.at(index);
        } else if (kind == "extractor") {
            auto fields = sample["fields"].toObject();
            if (run["scenario"].toString() == "failure") fields["quantity"] = "six";
            output = fields;
        } else if (kind == "pricing") output = price(inputs["fields"].toObject());
        else error = "This node has an unsupported operation.";
        if (error.isEmpty()) {
            step["output"] = output;
            for (const auto &value : outputPorts) {
                error = validate(output, value.toObject()["schema"].toObject(), "output");
                if (!error.isEmpty()) break;
            }
        }
    }
    if (!error.isEmpty()) step["error"] = error;
    finishStep(run, steps, step, eventAt, error.isEmpty() ? "passed" : "failed");
    return true;
}

QJsonObject BlueprintPreview::run(const QString &runId) const { return findById(m_runs, runId); }

QJsonArray BlueprintPreview::runs(const QString &blueprintId) const {
    QJsonArray result;
    for (const auto &value : m_runs)
        if (value.toObject()["blueprintId"].toString() == blueprintId) result.append(value);
    return result;
}

QJsonArray BlueprintPreview::chatRuns() const {
    QJsonArray result;
    for (const auto &value : m_runs)
        if (value.toObject()["source"].toString() == "Local chat") result.prepend(value);
    if (live()) while (result.size() > 20) result.removeFirst();
    return result;
}

void BlueprintPreview::tick() {
    const auto now = QDateTime::currentMSecsSinceEpoch();
    for (int i = 0; i < m_runs.size(); ++i) {
        auto run = m_runs.at(i).toObject();
        if (!advance(run, now)) continue;
        while (advance(run, now)) {}
        updateRun(i, run);
    }
}

void BlueprintPreview::updateRun(int index, const QJsonObject &run) {
    m_runs.replace(index, run);
    storeRuns();
    emit runChanged(run["id"].toString());
}

void BlueprintPreview::storeRuns() {
    if (m_persist) QSettings().setValue(runsKey, QJsonDocument(m_runs).toJson(QJsonDocument::Compact));
}

void BlueprintPreview::decide(const QString &id, bool approved, const QJsonObject &displayed) {
    if (live()) {
        decideLive(id, approved, displayed);
        return;
    }
    for (int i = 0; i < m_runs.size(); ++i) {
        auto run = m_runs.at(i).toObject();
        if (run["id"].toString() != id || run["status"].toString() != "waiting") continue;
        const auto now = QDateTime::currentMSecsSinceEpoch();
        if (!advance(run, now)) {
            const auto steps = run["steps"].toArray();
            auto step = steps.at(run["cursor"].toInt()).toObject();
            auto decision = run["decision"].toObject();
            decision["status"] = approved ? "approved" : "denied";
            run["decision"] = decision;
            if (approved) step["output"] = QJsonObject{{"decision", "approved"}, {"simulated", true}};
            finishStep(run, steps, step, now, approved ? "passed" : "denied");
        }
        updateRun(i, run);
        return;
    }
}

void BlueprintPreview::cancel(const QString &id) {
    if (live()) {
        cancelLive(id);
        return;
    }
    for (int i = 0; i < m_runs.size(); ++i) {
        auto run = m_runs.at(i).toObject();
        const auto status = run["status"].toString();
        if (run["id"].toString() != id || (status != "running" && status != "waiting")) continue;
        const auto now = QDateTime::currentMSecsSinceEpoch();
        if (status != "waiting" || !advance(run, now)) {
            auto decision = run["decision"].toObject();
            if (status == "waiting") {
                decision["status"] = "cancelled";
                run["decision"] = decision;
            }
            const auto steps = run["steps"].toArray();
            finishStep(run, steps, steps.at(run["cursor"].toInt()).toObject(), now, "cancelled");
        }
        updateRun(i, run);
        return;
    }
}

QString BlueprintPreview::statusLabel(const QString &status) {
    static const QHash<QString, QString> labels{{"running", "Running"}, {"waiting", "Waiting for approval"},
        {"passed", "Completed"}, {"failed", "Failed"}, {"cancelled", "Cancelled"}, {"denied", "Denied"},
        {"expired", "Expired"}, {"queued", "Queued"}, {"skipped", "Skipped"}, {"approved", "Approved once"},
        {"pending", "Pending"}, {"unavailable", "Unavailable"}, {"executing", "Executing"},
        {"completed", "Completed"}, {"interrupted", "Interrupted"}};
    return labels.value(status, status);
}

QString BlueprintPreview::duration(qint64 ms) {
    if (ms < 0) return QString::fromUtf8("—");
    return ms < 1000 ? QString("%1 ms").arg(ms) : QString("%1 s").arg(ms / 1000.0, 0, 'f', 2);
}

QString BlueprintPreview::jsonText(const QJsonValue &value) {
    if (value.isObject()) return QString::fromUtf8(QJsonDocument(value.toObject()).toJson()).trimmed();
    if (value.isUndefined()) return QString::fromUtf8("—");
    const auto json = QJsonDocument(QJsonArray{value}).toJson(QJsonDocument::Compact);
    return QString::fromUtf8(json.mid(1, json.size() - 2));
}

QJsonValue BlueprintPreview::parseJsonValue(const QString &text) {
    const auto document = QJsonDocument::fromJson("[" + text.toUtf8() + "]");
    if (document.isArray() && document.array().size() == 1) return document.array().first();
    return QJsonValue::Undefined;
}

bool BlueprintPreview::supportedSchema(const QJsonObject &schema) {
    const auto type = schema["type"].toString();
    if (type != "string" && type != "integer" && type != "number" && type != "object" &&
            type != "boolean" && type != "array" && type != "null") return false;
    if (schema.contains("minimum") && !schema["minimum"].isDouble()) return false;
    if (schema.contains("minLength") && schema["minLength"].toInt(-1) < 0) return false;
    if (schema.contains("required") && !schema["required"].isArray()) return false;
    for (const auto &key : schema["required"].toArray())
        if (!key.isString()) return false;
    if (schema.contains("items") && !supportedSchema(schema["items"].toObject())) return false;
    if (schema.contains("properties") && !schema["properties"].isObject()) return false;
    const auto props = schema["properties"].toObject();
    for (const auto &value : props)
        if (!supportedSchema(value.toObject())) return false;
    return true;
}
