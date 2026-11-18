#include "panels/blueprintnodedialog.h"

#include "panels/blueprintpreview.h"
#include "widgets/button.h"
#include "widgets/combobox.h"
#include "widgets/settingswindow.h"
#include "widgets/surface.h"

#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSet>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <algorithm>

namespace {
struct PortEditor {
    QJsonObject port;
    QString direction;
    QLineEdit *name;
    ComboBox *type;
    QLineEdit *key;
    QWidget *row;
};

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

class NodeDialog : public SettingsDetailDialog {
public:
    NodeDialog(BlueprintPreview &preview, QWidget *parent, const QString &kind, const QJsonObject &existing)
        : SettingsDetailDialog(parent), m_preview(preview), m_original(existing) {
        setObjectName("blueprintNodeDialog");
        setWindowTitle(existing.isEmpty() ? "Create node" : "Configure node");
        auto *body = new QVBoxLayout(this);
        body->setContentsMargins(20, 18, 20, 18);
        body->setSpacing(7);
        body->addWidget(caption(windowTitle(), "panelTitle"));
        m_kind = new ComboBox;
        m_operation = new ComboBox;
        for (const auto &value : m_preview.fixtures()["nodeTypes"].toArray()) {
            const auto entry = value.toObject();
            m_kind->addItem(entry["label"].toString(), entry["id"].toString());
        }
        m_kind->setCurrentIndex(qMax(0, m_kind->findData(kind)));
        Surface::field(body, "Node kind", m_kind);
        Surface::field(body, "What it does", m_operation);
        m_detail = caption({});
        body->addWidget(m_detail);
        m_name = new QLineEdit(existing["name"].toString());
        m_name->setMaxLength(60);
        Surface::field(body, "Name (optional)", m_name);

        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        auto *options = new QWidget;
        scroll->setWidget(options);
        m_options = new QVBoxLayout(options);
        m_options->setContentsMargins(0, 0, 0, 0);
        m_options->setSpacing(6);
        m_options->setAlignment(Qt::AlignTop);
        body->addWidget(scroll, 1);
        m_error = caption({}, "blueprintError");
        m_error->hide();
        body->addWidget(m_error);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
        buttons->addButton(windowTitle() == "Create node" ? "Create node" : "Save changes", QDialogButtonBox::AcceptRole);
        body->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, this, [this] { commit(); });
        connect(m_kind, &QComboBox::currentIndexChanged, this, [this] { loadOperations(); });
        connect(m_operation, &QComboBox::currentIndexChanged, this, [this] { loadOptions(); });
        loadOperations(BlueprintPreview::operationId(existing));
        m_operation->setFocus();
        setMinimumWidth(490);
        resize(520, 470);
        if (m_preview.live()) {
            connect(&m_preview, &BlueprintPreview::optionsChanged, this, [this] {
                if (m_binding) refreshBindings(m_binding->currentData().toString());
            });
            m_preview.refreshOptions();
        }
    }

    QJsonObject result() const { return m_result; }

private:
    void loadOperations(const QString &preferred = {}) {
        const QSignalBlocker blocker(m_operation);
        m_operation->clear();
        const auto kind = m_kind->currentData().toString();
        for (const auto &value : m_preview.fixtures()["operations"].toArray()) {
            const auto spec = value.toObject();
            const auto id = spec["id"].toString();
            if (spec["kind"].toString() != kind || (spec["legacyOnly"].toBool() && id != preferred)) continue;
            m_operation->addItem(spec["label"].toString(), id);
        }
        m_operation->setCurrentIndex(qMax(0, m_operation->findData(preferred)));
        m_name->setPlaceholderText(m_kind->currentText());
        loadOptions();
    }

    void loadOptions() {
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

        if (m_preview.live() && (code == "generate" || code == "reason" || code == "lora")) {
            m_binding = new ComboBox;
            m_binding->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
            m_binding->setMinimumContentsLength(20);
            const bool adapter = code == "lora";
            const QString key = adapter ? "adapter" : "model";
            refreshBindings(m_original["kind"] == spec["kind"] ? m_original[key].toString() : QString{});
            Surface::field(body, adapter ? "Trained adapter" : "Local checkpoint", m_binding);
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
            } else if (config["multiline"].toBool()) {
                m_text = new QPlainTextEdit(initial);
                m_text->setObjectName("blueprintCode");
                m_text->setPlaceholderText(config["placeholder"].toString());
                m_text->setFixedHeight(92);
                m_text->setTabChangesFocus(true);
                Surface::field(body, title, m_text);
            } else {
                m_line = new QLineEdit(initial);
                m_line->setPlaceholderText(config["placeholder"].toString());
                Surface::field(body, title, m_line);
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
            body->addWidget(caption(direction + " ports", "headingLabel"));
            if (editable) body->addWidget(caption("Key, display label, and data type."));
            auto *rows = new QVBoxLayout;
            body->addLayout(rows);
            for (const auto &value : ports) {
                const auto definition = value.toObject();
                auto port = sameOperation ? findPort(m_original[side].toArray(), definition["id"].toString()) : QJsonObject{};
                if (port.isEmpty()) port = definition;
                if (port["type"] == "adapter-ref") {
                    auto *note = caption(side == "in" ? "Optional connection from an Adapter node."
                                                       : "Connect this LoRA output to a Model or Reasoning node.");
                    rows->addWidget(note);
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
                    if (auto *scroll = findChild<QScrollArea *>()) scroll->ensureWidgetVisible(add);
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

    static bool editableKeys(const QString &operation, const QString &side) {
        return operation == "generate" || operation == "reason"
            || ((operation == "build-object" || operation == "template") && side == "in");
    }

    void refreshBindings(const QString &selection) {
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

    void appendPort(QVBoxLayout *rows, const QString &side, const QJsonObject &port, bool chooseType, bool editable) {
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
        }
        auto *name = new QLineEdit(port["name"].toString());
        name->setMaxLength(60);
        name->setPlaceholderText(port["name"].toString());
        name->setAccessibleName(direction + " port name");
        layout->addWidget(name, 1);
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
        if (editable) {
            auto *remove = new Button("−");
            remove->setAccessibleName("Remove " + direction.toLower() + " port");
            layout->addWidget(remove);
            connect(remove, &Button::clicked, row, [this, row] {
                m_ports.removeIf([row](const auto &editor) { return editor.row == row; });
                row->hide();
                row->deleteLater();
            });
        }
        m_ports.append({port, side, name, type, key, row});
    }

    void commit() {
        const auto code = m_operation->currentData().toString();
        if (code.isEmpty()) return;
        const auto nodeId = m_original["id"].toString(BlueprintPreview::id());
        auto node = code == BlueprintPreview::operationId(m_original)
            ? m_original : m_preview.makeNode(code, nodeId, m_original["x"].toInt(), m_original["y"].toInt());
        node["kind"] = m_kind->currentData().toString();
        node["operation"] = code;
        const auto name = m_name->text().trimmed();
        node["name"] = name.isEmpty() ? m_kind->currentText() : name;
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
        for (const auto &direction : {QString("in"), QString("out")}) {
            QJsonArray ports;
            QSet<QString> usedNames, usedKeys;
            for (const auto &editor : m_ports) {
                if (editor.direction != direction) continue;
                const auto customName = editor.name->text().trimmed();
                const auto portName = customName.isEmpty() ? editor.name->placeholderText() : customName;
                if (usedNames.contains(portName.toCaseFolded())) {
                    fail("Give each port on the same side a distinct name.");
                    editor.name->setFocus();
                    return;
                }
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
                        || (key == "adapter" && direction == m_resourceSide && !m_resourcePort.isEmpty())) {
                        fail("Use distinct port keys starting with a letter. Keys can contain numbers, underscores, and hyphens.");
                        editor.key->setFocus();
                        return;
                    }
                    usedKeys.insert(key);
                    port["id"] = key;
                }
                port["type"] = type;
                port["name"] = portName;
                ports.append(port);
            }
            if (editableKeys(code, direction) && ports.isEmpty()) {
                fail("Keep at least one " + QString(direction == "in" ? "input" : "output") + " data port.");
                return;
            }
            if (direction == m_resourceSide && !m_resourcePort.isEmpty()) ports.append(m_resourcePort);
            node[direction] = ports;
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
            if (outputs.size() != 1 || outputs.first().toObject()["type"] != "text") {
                fail("Plain text requires one text output. Change the response format in Change binding for typed outputs.");
                return;
            }
        }
        const auto error = m_preview.settingError(node);
        if (!error.isEmpty()) {
            fail(error);
            return;
        }
        m_result = node;
        accept();
    }

    void fail(const QString &message) {
        m_error->setText(message);
        m_error->show();
    }

    BlueprintPreview &m_preview;
    const QJsonObject m_original;
    QJsonObject m_result;
    ComboBox *m_kind;
    ComboBox *m_choice = nullptr;
    ComboBox *m_operation;
    QLineEdit *m_name;
    QLabel *m_detail;
    QLabel *m_error;
    QVBoxLayout *m_options;
    QWidget *m_fields = nullptr;
    QLineEdit *m_line = nullptr;
    ComboBox *m_binding = nullptr;
    QJsonObject m_resourcePort;
    QString m_resourceSide;
    QPlainTextEdit *m_text = nullptr;
    QList<PortEditor> m_ports;
};
}

std::optional<QJsonObject> configureBlueprintNode(BlueprintPreview &preview, QWidget *parent,
    const QString &kind, const QJsonObject &existing) {
    NodeDialog dialog(preview, parent, kind, existing);
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    return dialog.result();
}
