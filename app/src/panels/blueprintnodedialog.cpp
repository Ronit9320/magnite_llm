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
#include <QSignalBlocker>
#include <QVBoxLayout>

namespace {
struct PortEditor {
    QJsonObject port;
    QString direction;
    QLineEdit *name;
    ComboBox *type;
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

        auto *options = new QWidget;
        m_options = new QVBoxLayout(options);
        m_options->setContentsMargins(0, 0, 0, 0);
        m_options->setSpacing(6);
        body->addWidget(options);
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
        setMinimumWidth(450);
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
        m_line = nullptr;
        m_text = nullptr;
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
        const auto fixtures = m_preview.fixtures();
        const auto contracts = fixtures["contracts"].toObject();
        m_detail->setText(spec["detail"].toString());

        const auto config = spec["setting"].toObject();
        if (!config.isEmpty()) {
            const auto title = config["label"].toString();
            const auto initial = sameOperation ? m_original["setting"].toString() : config["default"].toString();
            if (config["multiline"].toBool()) {
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
                    port["chooseType"] = definition["chooseType"].toBool() || code == "generate" || code == "reason";
                    ports.replace(i, port);
                }
            }
            if (ports.isEmpty()) continue;
            const QString direction = side == "in" ? "Input" : "Output";
            body->addWidget(caption(direction + " ports", "headingLabel"));
            for (int index = 0; index < ports.size(); ++index) {
                const auto definition = ports.at(index).toObject();
                auto port = sameOperation ? findPort(m_original[side].toArray(), definition["id"].toString()) : QJsonObject{};
                if (port.isEmpty()) port = definition;
                const QString suffix = ports.size() == 1 ? QString{} : QString(" %1").arg(index + 1);
                auto *row = new QHBoxLayout;
                auto *name = new QLineEdit(port["name"].toString());
                name->setMaxLength(60);
                name->setPlaceholderText(definition["name"].toString());
                name->setAccessibleName(direction + " port name" + suffix);
                row->addWidget(name, 1);
                auto *type = new ComboBox;
                type->setAccessibleName(direction + " port type" + suffix);
                const auto selected = port["type"].toString();
                if (definition["chooseType"].toBool()) {
                    for (const auto &value : fixtures["portTypes"].toArray()) {
                        const auto id = value.toString();
                        type->addItem(contracts.value(id).toObject()["label"].toString(), id);
                    }
                    if (type->findData(selected) < 0)
                        type->addItem(contracts.value(selected).toObject()["label"].toString(), selected);
                    type->setCurrentIndex(type->findData(selected));
                } else {
                    type->addItem(contracts.value(selected).toObject()["label"].toString(), selected);
                    type->setEnabled(false);
                }
                row->addWidget(type);
                body->addLayout(row);
                m_ports.append({port, side, name, type});
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
        adjustSize();
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
        const auto contracts = m_preview.fixtures()["contracts"].toObject();
        for (const auto &direction : {QString("in"), QString("out")}) {
            QJsonArray ports;
            QSet<QString> usedNames;
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
                port["type"] = type;
                port["name"] = portName;
                ports.append(port);
            }
            node[direction] = ports;
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
        adjustSize();
    }

    BlueprintPreview &m_preview;
    const QJsonObject m_original;
    QJsonObject m_result;
    ComboBox *m_kind;
    ComboBox *m_operation;
    QLineEdit *m_name;
    QLabel *m_detail;
    QLabel *m_error;
    QVBoxLayout *m_options;
    QWidget *m_fields = nullptr;
    QLineEdit *m_line = nullptr;
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
