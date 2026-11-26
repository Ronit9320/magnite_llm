#include "panels/blueprintnodefields.h"

#include "panels/blueprintpreview.h"
#include "widgets/button.h"
#include "widgets/combobox.h"
#include "widgets/surface.h"

#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSet>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>

namespace {
QLabel *caption(const QString &text, const char *style = "cardMeta") {
    auto *label = new QLabel(text);
    label->setTextFormat(Qt::PlainText);
    label->setObjectName(style);
    label->setWordWrap(true);
    return label;
}

QJsonObject findPort(const QJsonArray &ports, const QString &id) {
    for (const auto &value : ports) {
        const auto port = value.toObject();
        if (port["id"].toString() == id) return port;
    }
    return {};
}

QScrollArea *scrollAreaOf(QWidget *widget) {
    for (auto *parent = widget ? widget->parentWidget() : nullptr; parent; parent = parent->parentWidget())
        if (auto *scroll = qobject_cast<QScrollArea *>(parent)) return scroll;
    return nullptr;
}
}  // namespace

BlueprintNodeFields::BlueprintNodeFields(BlueprintPreview &preview, const QString &kind, const QJsonObject &node,
                                         QWidget *parent)
    : QWidget(parent), m_preview(preview), m_original(node) {
    setObjectName("blueprintNodeFields");
    auto *body = new QVBoxLayout(this);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(7);
    m_kind = new ComboBox;
    m_operation = new ComboBox;
    for (const auto &value : m_preview.fixtures()["nodeTypes"].toArray()) {
        const auto entry = value.toObject();
        m_kind->addItem(entry["label"].toString(), entry["id"].toString());
    }
    // A kind the catalog no longer lists stays selected, so editing never turns the node into another one.
    if (!kind.isEmpty() && m_kind->findData(kind) < 0) m_kind->addItem("Unavailable · " + kind, kind);
    m_kind->setCurrentIndex(qMax(0, m_kind->findData(kind)));
    Surface::field(body, "Node kind", m_kind);
    Surface::field(body, "What it does", m_operation);
    m_detail = caption({});
    body->addWidget(m_detail);
    m_name = new QLineEdit(node["name"].toString());
    m_name->setMaxLength(60);
    Surface::field(body, "Name (optional)", m_name);
    watchLine(m_name);

    auto *options = new QWidget;
    m_options = new QVBoxLayout(options);
    m_options->setContentsMargins(0, 0, 0, 0);
    m_options->setSpacing(6);
    m_options->setAlignment(Qt::AlignTop);
    body->addWidget(options, 1);
    m_error = caption({}, "blueprintError");
    m_error->hide();
    body->addWidget(m_error);

    connect(m_kind, &QComboBox::currentIndexChanged, this, [this] {
        loadOperations();
        emit edited();
    });
    connect(m_operation, &QComboBox::currentIndexChanged, this, [this] {
        // A name that was only the previous default follows the newly chosen operation.
        if (m_name->text() == m_name->placeholderText()) m_name->clear();
        m_name->setPlaceholderText(defaultName());
        loadOptions();
        emit edited();
    });
    loadOperations(BlueprintPreview::operationId(node));
    if (m_preview.live()) {
        connect(&m_preview, &BlueprintPreview::optionsChanged, this, [this] {
            if (m_binding) refreshBindings(m_binding->currentData().toString());
        });
        m_preview.refreshOptions();
    }
}

void BlueprintNodeFields::watchLine(QLineEdit *edit) {
    connect(edit, &QLineEdit::editingFinished, this, [this, edit] {
        if (edit->isModified()) {
            edit->setModified(false);
            emit edited();
        }
    });
}

bool BlueprintNodeFields::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_text && event->type() == QEvent::FocusOut && m_text->document()->isModified()) {
        m_text->document()->setModified(false);
        emit edited();
    }
    return QWidget::eventFilter(watched, event);
}

void BlueprintNodeFields::loadOperations(const QString &preferred) {
    const QSignalBlocker blocker(m_operation);
    m_operation->clear();
    const auto kind = m_kind->currentData().toString();
    for (const auto &value : m_preview.fixtures()["operations"].toArray()) {
        const auto spec = value.toObject();
        const auto id = spec["id"].toString();
        if (spec["kind"].toString() != kind || (spec["legacyOnly"].toBool() && id != preferred)) continue;
        m_operation->addItem(spec["label"].toString(), id);
    }
    if (!preferred.isEmpty() && m_operation->findData(preferred) < 0
        && kind == m_original["kind"].toString(m_preview.operation(preferred)["kind"].toString()))
        m_operation->addItem("Unavailable · " + preferred, preferred);
    m_operation->setCurrentIndex(qMax(0, m_operation->findData(preferred)));
    m_name->setPlaceholderText(defaultName());
    loadOptions();
}

void BlueprintNodeFields::loadOptions() {
    m_ports.clear();
    m_resourcePort = {};
    m_resourceSide.clear();
    m_line = nullptr;
    m_binding = nullptr;
    m_text = nullptr;
    m_choice = nullptr;
    m_error->hide();
    if (m_fields) {
        m_options->removeWidget(m_fields);
        delete m_fields;
    }
    m_fields = new QWidget;
    m_options->addWidget(m_fields);
    auto *body = new QVBoxLayout(m_fields);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(5);
    const auto code = m_operation->currentData().toString();
    const auto spec = m_preview.operation(code);
    const bool sameOperation = code == BlueprintPreview::operationId(m_original);
    m_detail->setText(spec["detail"].toString());

    if (m_bindingShown && m_preview.live() && (code == "generate" || code == "reason" || code == "lora")) {
        m_binding = new ComboBox;
        m_binding->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        m_binding->setMinimumContentsLength(12);
        const bool adapter = code == "lora";
        const QString key = adapter ? "adapter" : "model";
        refreshBindings(m_original["kind"] == spec["kind"] ? m_original[key].toString() : QString{});
        Surface::field(body, adapter ? "Trained adapter" : "Local checkpoint", m_binding);
        connect(m_binding, &QComboBox::currentIndexChanged, this, &BlueprintNodeFields::edited);
    }
    const auto config = spec["setting"].toObject();
    if (!config.isEmpty()) {
        const auto title = config["label"].toString();
        const auto initial = sameOperation ? m_original["setting"].toString() : config["default"].toString();
        if (config["options"].isArray()) {
            m_choice = new ComboBox;
            m_choice->setAccessibleName(title);
            for (const auto &value : config["options"].toArray())
                m_choice->addItem(value.toArray().at(1).toString(), value.toArray().at(0).toString());
            m_choice->setCurrentIndex(qMax(0, m_choice->findData(initial)));
            Surface::field(body, title, m_choice);
            connect(m_choice, &QComboBox::currentIndexChanged, this, &BlueprintNodeFields::edited);
        } else if (config["multiline"].toBool()) {
            m_text = new QPlainTextEdit(initial);
            m_text->setObjectName("blueprintCode");
            m_text->setPlaceholderText(config["placeholder"].toString());
            m_text->setFixedHeight(92);
            m_text->setTabChangesFocus(true);
            m_text->installEventFilter(this);
            Surface::field(body, title, m_text);
        } else {
            m_line = new QLineEdit(initial);
            m_line->setPlaceholderText(config["placeholder"].toString());
            Surface::field(body, title, m_line);
            watchLine(m_line);
        }
    }

    for (const auto &side : {QString("in"), QString("out")}) {
        auto ports = spec[side].toArray();
        // A saved checkpoint or imported transform can declare named ports
        // beyond the defaults used for newly created nodes.
        if (m_preview.live() && sameOperation) {
            ports = m_original[side].toArray();
            for (int i = 0; i < ports.size(); ++i) {
                auto port = ports.at(i).toObject();
                const auto definition = findPort(spec[side].toArray(), port["id"].toString());
                port["chooseType"] = definition["chooseType"].toBool() || editableKeys(code, side);
                ports.replace(i, port);
            }
        }
        if (ports.isEmpty()) continue;
        const QString direction = side == "in" ? "Input" : "Output";
        const bool editable = editableKeys(code, side);
        const bool dataPorts = std::any_of(ports.begin(), ports.end(), [](const QJsonValue &value) {
            return value.toObject()["type"] != "adapter-ref";
        });
        if (!dataPorts && !m_helpShown) {
            // Only the LoRA connection: nothing to edit here, the Inputs and Outputs rows show it.
            for (const auto &value : ports) {
                m_resourcePort = sameOperation ? findPort(m_original[side].toArray(), value.toObject()["id"].toString()) : QJsonObject{};
                if (m_resourcePort.isEmpty()) m_resourcePort = value.toObject();
                m_resourceSide = side;
            }
            continue;
        }
        body->addWidget(caption(direction + " ports", m_helpShown ? "headingLabel" : "cardMeta"));
        if (editable && m_helpShown) body->addWidget(caption("Key, display label, and data type."));
        auto *rows = new QVBoxLayout;
        body->addLayout(rows);
        for (const auto &value : ports) {
            const auto definition = value.toObject();
            auto port = sameOperation ? findPort(m_original[side].toArray(), definition["id"].toString()) : QJsonObject{};
            if (port.isEmpty()) port = definition;
            if (port["type"] == "adapter-ref") {
                if (m_helpShown)
                    rows->addWidget(caption(side == "in" ? "Optional connection from an Adapter node."
                                                         : "Connect this LoRA output to a Model or Reasoning node."));
                m_resourcePort = port;
                m_resourceSide = side;
                continue;
            }
            appendPort(rows, side, port, definition["chooseType"].toBool(), editable);
        }
        if (editable) {
            auto *add = new Button("Add " + direction.toLower() + " port");
            body->addWidget(add, 0, Qt::AlignLeft);
            connect(add, &Button::clicked, m_fields, [this, add, rows, side] {
                QSet<QString> keys;
                for (const auto &editor : m_ports)
                    if (editor.direction == side && editor.key) keys.insert(editor.key->text().trimmed());
                if (std::count_if(m_ports.begin(), m_ports.end(), [&](const auto &editor) {
                    return editor.direction == side;
                }) >= 16) {
                    fail("Use at most 16 data ports per side.");
                    return;
                }
                const QString prefix = side == "in" ? "input" : "output";
                int number = 1;
                while (keys.contains(prefix + QString::number(number))) ++number;
                const auto key = prefix + QString::number(number);
                appendPort(rows, side, {{"id", key}, {"name", key}, {"type", "text"}}, true, true);
                m_ports.last().key->setFocus();
                m_ports.last().key->selectAll();
                if (auto *scroll = scrollAreaOf(add)) scroll->ensureWidgetVisible(add);
                emit edited();
            });
        }
    }
    if (spec["sameType"].toBool() && m_ports.size() == 2) {
        auto *input = m_ports.first().type;
        auto *output = m_ports.last().type;
        auto syncType = [input, output] { output->setCurrentIndex(output->findData(input->currentData())); };
        connect(input, &QComboBox::currentIndexChanged, output, syncType);
        syncType();
        output->setEnabled(false);
        output->setToolTip("This operation returns the same type it receives.");
    }
}

QString BlueprintNodeFields::defaultName() const {
    const auto kind = m_kind->currentData().toString();
    return kind == "transform" || kind == "knowledge" ? m_operation->currentText() : m_kind->currentText();
}

bool BlueprintNodeFields::editableKeys(const QString &operation, const QString &side) {
    return operation == "generate" || operation == "reason"
        || ((operation == "build-object" || operation == "template") && side == "in");
}

void BlueprintNodeFields::refreshBindings(const QString &selection) {
    const QSignalBlocker blocked(m_binding);
    const bool adapter = m_operation->currentData() == "lora";
    m_binding->clear();
    m_binding->addItem(adapter ? "Choose a trained adapter" : "Choose a local checkpoint", "");
    const bool reasoning = m_operation->currentData() == "reason";
    for (const auto &value : m_preview.fixtures()[adapter ? "adapters" : "models"].toArray()) {
        const auto option = value.toObject();
        auto title = option["name"].toString();
        if (adapter && option["base_model"].isString()) title += " · " + option["base_model"].toString();
        // Reasoning needs a checkpoint whose chat template can switch thinking on.
        const bool unsupported = reasoning && option["thinking"] == false;
        m_binding->addItem(unsupported ? title + " · no thinking" : title, option["id"].toString());
        if (unsupported) BlueprintPreview::disableItem(m_binding, m_binding->count() - 1);
    }
    if (!selection.isEmpty() && m_binding->findData(selection) < 0)
        m_binding->addItem("Unavailable · " + selection, selection);
    m_binding->setCurrentIndex(qMax(0, m_binding->findData(selection)));
    if (m_binding->count() == 1)
        m_binding->setItemText(0, adapter ? "No trained adapters. See Adapters." : "No ready checkpoints. See Models.");
}

void BlueprintNodeFields::appendPort(QVBoxLayout *rows, const QString &side, const QJsonObject &port, bool chooseType,
                                     bool editable) {
    auto *row = new QWidget;
    auto *layout = new QHBoxLayout(row);
    layout->setContentsMargins(0, 0, 0, 0);
    rows->addWidget(row);
    const QString direction = side == "in" ? "Input" : "Output";
    QLineEdit *key = nullptr;
    if (editable) {
        key = new QLineEdit(port["id"].toString());
        key->setMaxLength(64);
        key->setAccessibleName(direction + " port key");
        key->setToolTip("Key used in JSON and connections");
        layout->addWidget(key, 1);
        watchLine(key);
    }
    auto *name = new QLineEdit(port["name"].toString());
    name->setMaxLength(60);
    name->setPlaceholderText(m_preview.portLabel(port));
    name->setAccessibleName(direction + " port name");
    layout->addWidget(name, 1);
    watchLine(name);
    auto *type = new ComboBox;
    type->setAccessibleName(direction + " port type");
    type->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    type->setMinimumContentsLength(8);
    const auto fixtures = m_preview.fixtures();
    const auto contracts = fixtures["contracts"].toObject();
    const auto selected = port["type"].toString();
    if (chooseType) {
        for (const auto &value : fixtures["portTypes"].toArray()) {
            const auto id = value.toString();
            type->addItem(contracts.value(id).toObject()["label"].toString(), id);
        }
    }
    if (type->findData(selected) < 0)
        type->addItem(contracts.value(selected).toObject()["label"].toString(), selected);
    type->setCurrentIndex(type->findData(selected));
    type->setEnabled(chooseType);
    layout->addWidget(type);
    connect(type, &QComboBox::currentIndexChanged, this, &BlueprintNodeFields::edited);
    if (editable) {
        auto *remove = new Button("−");
        remove->setAccessibleName("Remove " + direction.toLower() + " port");
        layout->addWidget(remove);
        connect(remove, &Button::clicked, row, [this, row] {
            m_ports.removeIf([row](const auto &editor) { return editor.row == row; });
            row->hide();
            row->deleteLater();
            emit edited();
        });
    }
    m_ports.append({port, side, name, type, key, row});
}

QJsonObject BlueprintNodeFields::build(QString *error, QWidget **focus) const {
    const auto fail = [&](const QString &message, QWidget *widget = nullptr) {
        if (error) *error = message;
        if (focus) *focus = widget;
        return QJsonObject{};
    };
    const auto code = m_operation->currentData().toString();
    if (code.isEmpty()) return fail("Choose what this node does.", m_operation);
    const auto nodeId = m_original["id"].toString(BlueprintPreview::id());
    auto node = code == BlueprintPreview::operationId(m_original)
        ? m_original : m_preview.makeNode(code, nodeId, m_original["x"].toInt(), m_original["y"].toInt());
    node["kind"] = m_kind->currentData().toString();
    node["operation"] = code;
    const auto name = m_name->text().trimmed();
    node["name"] = name.isEmpty() ? defaultName() : name;
    if (m_line) node["setting"] = m_line->text();
    if (m_text) node["setting"] = m_text->toPlainText();
    if (m_choice) node["setting"] = m_choice->currentData().toString();
    if (m_binding) {
        const auto choice = m_binding->currentData().toString();
        if (code == "lora") node["adapter"] = choice;
        else {
            if (node["model"].toString() != choice) node.remove("adapter");
            node["model"] = choice;
        }
    }
    const auto contracts = m_preview.fixtures()["contracts"].toObject();
    // An operation the catalog doesn't describe has no port editors; its ports stay as they were.
    const bool knownOperation = !m_preview.operation(code).isEmpty();
    for (const auto &direction : {QString("in"), QString("out")}) {
        if (!knownOperation) continue;
        QJsonArray ports;
        QSet<QString> usedNames, usedKeys;
        for (const auto &editor : m_ports) {
            if (editor.direction != direction) continue;
            const auto customName = editor.name->text().trimmed();
            const auto portName = customName.isEmpty() ? editor.name->placeholderText() : customName;
            if (usedNames.contains(portName.toCaseFolded()))
                return fail("Give each port on the same side a distinct name.", editor.name);
            usedNames.insert(portName.toCaseFolded());
            auto port = editor.port;
            const auto type = editor.type->currentData().toString();
            if (port["type"].toString() != type || !port.contains("schema"))
                port["schema"] = contracts.value(type).toObject()["schema"];
            port.remove("chooseType");
            if (editor.key) {
                const auto key = editor.key->text().trimmed();
                static const QRegularExpression validKey("^[A-Za-z][A-Za-z0-9_-]{0,63}$");
                if (!validKey.match(key).hasMatch() || usedKeys.contains(key)
                    || (key == "adapter" && direction == m_resourceSide && !m_resourcePort.isEmpty()))
                    return fail("Use distinct port keys starting with a letter. Keys can contain numbers, underscores, and hyphens.",
                                editor.key);
                usedKeys.insert(key);
                port["id"] = key;
            }
            port["type"] = type;
            port["name"] = portName;
            ports.append(port);
        }
        if (editableKeys(code, direction) && ports.isEmpty())
            return fail("Keep at least one " + QString(direction == "in" ? "input" : "output") + " data port.");
        if (direction == m_resourceSide && !m_resourcePort.isEmpty()) ports.append(m_resourcePort);
        node[direction] = ports;
    }
    if (m_preview.operation(code)["sameType"].toBool() && node["in"].toArray().size() == 1 && node["out"].toArray().size() == 1) {
        // The output takes the input's type, whatever order the type pickers reported in.
        auto outputs = node["out"].toArray();
        auto output = outputs.first().toObject();
        const auto input = node["in"].toArray().first().toObject();
        output["type"] = input["type"];
        output["schema"] = input["schema"];
        outputs.replace(0, output);
        node["out"] = outputs;
    }
    if (code == "validate") {
        // The rules define both ports, so a connection shows the validated shape.
        const auto rules = BlueprintPreview::parseJsonValue(node["setting"].toString()).toObject();
        const auto type = rules["type"].toString();
        if (!rules.isEmpty() && !type.isEmpty()) {
            const auto editorType = type == "string" ? QString("text") : type;
            for (const auto &direction : {QString("in"), QString("out")}) {
                auto ports = node[direction].toArray();
                auto port = ports.first().toObject();
                port["type"] = editorType;
                port["schema"] = direction == "out" ? rules : QJsonObject{{"type", type}};
                ports.replace(0, port);
                node[direction] = ports;
            }
        }
    }
    if (node["response"] == "text") {
        const auto outputs = node["out"].toArray();
        if (outputs.size() != 1 || outputs.first().toObject()["type"] != "text")
            return fail("Plain text requires one text output. Change the response format in Change binding for typed outputs.");
    }
    const auto settingError = m_preview.settingError(node);
    if (!settingError.isEmpty()) return fail(settingError, m_line ? static_cast<QWidget *>(m_line) : m_text);
    if (error) error->clear();
    return node;
}

QWidget *BlueprintNodeFields::firstField() const { return m_operation; }

void BlueprintNodeFields::setHelpShown(bool shown) {
    if (m_helpShown == shown) return;
    m_helpShown = shown;
    m_detail->setVisible(shown);
    loadOptions();
}

void BlueprintNodeFields::setBindingShown(bool shown) {
    if (m_bindingShown == shown) return;
    m_bindingShown = shown;
    loadOptions();
}

QJsonObject BlueprintNodeFields::node() const { return build(nullptr, nullptr); }

QString BlueprintNodeFields::error() const {
    QString message;
    build(&message, nullptr);
    return message;
}

bool BlueprintNodeFields::showError() {
    QString message;
    QWidget *focus = nullptr;
    build(&message, &focus);
    if (message.isEmpty()) {
        m_error->hide();
        return false;
    }
    fail(message);
    if (focus) focus->setFocus();
    return true;
}

bool BlueprintNodeFields::portsChanged() const {
    const auto edited = node();
    if (edited.isEmpty()) return false;
    for (const auto &side : {QString("in"), QString("out")}) {
        const auto before = m_original[side].toArray();
        const auto after = edited[side].toArray();
        if (before.size() != after.size()) return true;
        for (int i = 0; i < before.size(); ++i) {
            const auto a = before.at(i).toObject();
            const auto b = after.at(i).toObject();
            if (a["id"] != b["id"] || a["type"] != b["type"]) return true;
        }
    }
    return false;
}

void BlueprintNodeFields::fail(const QString &message) {
    m_error->setText(message);
    m_error->show();
}
