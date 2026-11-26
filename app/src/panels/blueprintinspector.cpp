#include "panels/blueprintinspector.h"

#include "panels/blueprintbindingfields.h"
#include "panels/blueprintnodefields.h"
#include "panels/blueprintpreview.h"
#include "theme.h"
#include "widgets/blueprintscene.h"
#include "widgets/blueprintstyle.h"
#include "widgets/button.h"

#include <QApplication>
#include <QComboBox>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTimer>
#include <QVBoxLayout>
#include <utility>

namespace {
QLabel *text(const QString &value, const char *style = "cardMeta", bool wrap = true) {
    auto *label = new QLabel(value);
    label->setTextFormat(Qt::PlainText);
    label->setObjectName(style);
    label->setWordWrap(wrap);
    if (wrap) label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return label;
}

QFrame *rule() {
    auto *line = new QFrame;
    line->setObjectName("blueprintRule");
    line->setFixedHeight(1);
    return line;
}

Button *iconButton(const QString &icon, const QString &name) {
    auto *button = new Button({});
    button->setFlat(true);
    button->setObjectName("blueprintIconButton");
    button->setAccessibleName(name);
    button->setToolTip(name);
    button->setFixedSize(32, 32);
    button->setProperty("themeIcon", icon);
    button->setIcon(Theme::coloredIcon(icon, QColor(Theme::textSecondary()), 16));
    return button;
}

QJsonObject findById(const QJsonArray &array, const QString &id) {
    for (const auto &value : array)
        if (value.toObject()["id"].toString() == id) return value.toObject();
    return {};
}

// A text edit holds typing that has not been committed yet.
bool pendingText(const QWidget *section) {
    if (!section) return false;
    for (const auto *edit : section->findChildren<QLineEdit *>())
        if (edit->isModified()) return true;
    for (const auto *edit : section->findChildren<QPlainTextEdit *>())
        if (edit->document()->isModified()) return true;
    return false;
}

void markCommitted(QWidget *section) {
    if (!section) return;
    for (auto *edit : section->findChildren<QLineEdit *>()) edit->setModified(false);
    for (auto *edit : section->findChildren<QPlainTextEdit *>()) edit->document()->setModified(false);
}

// Where focus sat inside the inspector, so a rebuild after a save can put it back.
struct FocusSpot {
    QString name;
    QByteArray type;
    int index = -1;
    int cursor = -1;
};

FocusSpot focusSpot(QWidget *root) {
    auto *focus = QApplication::focusWidget();
    if (!focus || !root->isAncestorOf(focus) || focus->accessibleName().isEmpty()) return {};
    FocusSpot spot{focus->accessibleName(), focus->metaObject()->className()};
    int index = 0;
    for (auto *widget : root->findChildren<QWidget *>()) {
        if (widget->accessibleName() != spot.name || widget->metaObject()->className() != spot.type) continue;
        if (widget == focus) { spot.index = index; break; }
        ++index;
    }
    if (auto *edit = qobject_cast<QLineEdit *>(focus)) spot.cursor = edit->cursorPosition();
    return spot;
}

void restoreFocus(QWidget *root, const FocusSpot &spot) {
    if (spot.index < 0) return;
    int index = 0;
    for (auto *widget : root->findChildren<QWidget *>()) {
        if (widget->accessibleName() != spot.name || widget->metaObject()->className() != spot.type) continue;
        if (index++ != spot.index) continue;
        widget->setFocus();
        if (auto *edit = qobject_cast<QLineEdit *>(widget); edit && spot.cursor >= 0) edit->setCursorPosition(spot.cursor);
        return;
    }
}
}  // namespace

BlueprintInspector::BlueprintInspector(BlueprintPreview &preview, QWidget *parent)
    : QWidget(parent), m_preview(preview) {
    setObjectName("blueprintNodeInspector");
    m_root = new QVBoxLayout(this);
    m_root->setContentsMargins(0, 0, 0, 0);
    m_root->setSpacing(0);
}

void BlueprintInspector::setNode(const QJsonObject &graph, const QString &nodeId) {
    if (nodeId != m_nodeId) flush();
    const auto node = findById(graph["nodes"].toArray(), nodeId);
    const bool same = nodeId == m_nodeId && node == m_node && graph["edges"] == m_graph["edges"]
        && graph["revision"] == m_graph["revision"] && graph["dirty"] == m_graph["dirty"];
    m_graph = graph;
    m_nodeId = node.isEmpty() ? QString{} : nodeId;
    m_node = node;
    if (same && m_content) return;
    rebuild();
}

void BlueprintInspector::reload() { rebuild(); }

void BlueprintInspector::flush() {
    // Typing that was never committed belongs to the node it was typed for. The widgets go
    // away with the rebuild, so the node is read now and reported once the caller is done.
    QList<QPair<QJsonObject, bool>> pending;
    if (pendingText(m_fields)) {
        const auto node = m_fields->node();
        if (!node.isEmpty() && node != m_node) pending.append({node, m_fields->portsChanged()});
        markCommitted(m_fields);
    }
    if (pendingText(m_binding)) {
        if (m_binding->ready() && m_binding->error().isEmpty() && m_binding->node() != m_node)
            pending.append({m_binding->node(), false});
        markCommitted(m_binding);
    }
    if (pendingText(m_dataset)) {
        if (m_dataset->error().isEmpty() && m_dataset->node() != m_node) pending.append({m_dataset->node(), false});
        markCommitted(m_dataset);
    }
    for (const auto &[node, ports] : pending)
        QTimer::singleShot(0, this, [this, node, ports] { emit nodeChanged(node, ports); });
}

void BlueprintInspector::rebuild() {
    const auto spot = m_content ? focusSpot(m_content) : FocusSpot{};
    if (m_content) {
        // Deleted later: a rebuild can run inside a signal from one of these fields.
        m_root->removeWidget(m_content);
        m_content->hide();
        m_content->deleteLater();
    }
    m_fields = nullptr;
    m_binding = nullptr;
    m_bindingPending = false;
    m_dataset = nullptr;
    m_bindingError = nullptr;
    m_content = new QWidget;
    m_root->addWidget(m_content);
    if (m_node.isEmpty()) return;
    auto *body = new QVBoxLayout(m_content);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(7);
    const auto id = m_nodeId;
    const auto code = BlueprintPreview::operationId(m_node);
    const auto spec = m_preview.operation(code);
    const auto kind = spec["kind"].toString(m_node["kind"].toString());

    auto *header = new QHBoxLayout;
    header->addWidget(text("Node inspector", "headingLabel", false), 1);
    auto *details = iconButton("info", "Node details");
    auto *close = iconButton("window-close", "Clear node selection");
    close->setFixedSize(24, 24);
    header->addWidget(details);
    header->addWidget(close);
    connect(details, &Button::clicked, this, [this, id] { emit detailsRequested(id); });
    connect(close, &Button::clicked, this, &BlueprintInspector::closeRequested);
    body->addLayout(header);
    body->addWidget(rule());

    const auto iconKind = code == "selector" || code == "extractor" ? code : kind;
    auto *identity = new QHBoxLayout;
    identity->setSpacing(9);
    auto *icon = text({}, "blueprintNodeIcon", false);
    icon->setFixedSize(34, 34);
    icon->setAlignment(Qt::AlignCenter);
    icon->setPixmap(Theme::coloredIcon(BlueprintStyle::nodeIcon(iconKind),
        BlueprintStyle::familyColor(BlueprintStyle::nodeFamily(iconKind)), 19).pixmap(19, 19));
    identity->addWidget(icon);
    auto *names = new QVBoxLayout;
    names->setSpacing(3);
    names->addWidget(text(m_node["name"].toString(), "blueprintSection"));
    names->addWidget(text(m_preview.nodeKindLabel(kind) + " · " + spec["label"].toString()));
    identity->addLayout(names, 1);
    body->addLayout(identity);
    if (!spec["detail"].toString().isEmpty()) body->addWidget(text(spec["detail"].toString()));
    const auto warnings = BlueprintScene::warningLines(m_preview, m_graph, m_node, m_preview.issues(m_graph));
    if (!warnings.isEmpty()) {
        auto *warning = text(warnings.join('\n'), "blueprintError");
        warning->setAccessibleName("Node warnings");
        body->addWidget(warning);
    }

    body->addWidget(rule());
    const bool liveModel = m_preview.live() && (kind == "model" || kind == "reasoning");
    m_fields = new BlueprintNodeFields(m_preview, kind, m_node);
    if (liveModel) m_fields->setBindingShown(false);
    body->addWidget(m_fields);
    connect(m_fields, &BlueprintNodeFields::edited, this, &BlueprintInspector::commitNodeFields);

    if (liveModel || (!m_preview.live() && kind == "model")) {
        body->addWidget(rule());
        body->addWidget(text(kind == "reasoning" ? "Reasoning checkpoint" : "Model & adapter", "headingLabel", false));
        m_binding = new BlueprintBindingFields(m_preview, m_graph, m_node);
        body->addWidget(m_binding);
        m_bindingError = text({}, "blueprintError");
        m_bindingError->hide();
        body->addWidget(m_bindingError);
        connect(m_binding, &BlueprintBindingFields::edited, this, &BlueprintInspector::commitBinding);
        connect(m_binding, &BlueprintBindingFields::readyChanged, this, [this](bool ready) {
            if (ready && std::exchange(m_bindingPending, false)) commitBinding();
        });
        if (liveModel) {
            auto *train = new Button("Training jobs…");
            train->setFlat(true);
            train->setEnabled(m_graph["revision"].toInteger() > 0 && !m_graph["dirty"].toBool());
            if (!train->isEnabled()) train->setToolTip("Save a complete revision before training or binding a job.");
            body->addWidget(train, 0, Qt::AlignLeft);
            connect(train, &Button::clicked, this, [this, id] { emit trainRequested(id); });
        } else {
            const auto model = findById(m_preview.fixtures()["models"].toArray(), m_node["model"].toString());
            body->addWidget(text(model["engine"].toString(), "blueprintCode", false));
            auto *train = new Button("Train adapter");
            train->setFlat(true);
            body->addWidget(train, 0, Qt::AlignLeft);
            connect(train, &Button::clicked, this, [this, id] { emit trainRequested(id); });
            const auto recipe = m_node["training"].toObject();
            if (!recipe.isEmpty()) body->addWidget(text("Training recipe saved\n" + recipe["name"].toString()));
        }
    } else if (m_preview.live() && kind == "dataset") {
        body->addWidget(rule());
        body->addWidget(text("Dataset", "headingLabel", false));
        m_dataset = new BlueprintDatasetFields(m_preview, m_node);
        body->addWidget(m_dataset);
        m_bindingError = text({}, "blueprintError");
        m_bindingError->hide();
        body->addWidget(m_bindingError);
        connect(m_dataset, &BlueprintDatasetFields::edited, this, &BlueprintInspector::commitDataset);
    }

    body->addWidget(rule());
    addPorts(body);
    body->addWidget(rule());
    auto *actions = new QHBoxLayout;
    auto *test = new Button("Test this node");
    auto *save = iconButton("folder", "Save to node library");
    auto *remove = iconButton("trash", "Delete node");
    actions->addWidget(test);
    actions->addStretch();
    actions->addWidget(save);
    actions->addWidget(remove);
    body->addLayout(actions);
    connect(test, &Button::clicked, this, [this, id] { emit testRequested(id); });
    connect(save, &Button::clicked, this, [this, id] { emit saveToLibraryRequested(id); });
    connect(remove, &Button::clicked, this, [this, id] { emit deleteRequested(id); });
    body->addStretch();
    restoreFocus(m_content, spot);
}

void BlueprintInspector::addPorts(QVBoxLayout *body) {
    body->addWidget(text("Inputs & outputs", "headingLabel", false));
    const auto contracts = m_preview.fixtures()["contracts"].toObject();
    const auto nodes = m_graph["nodes"].toArray();
    const auto edges = m_graph["edges"].toArray();
    for (const auto &side : {QString("in"), QString("out")}) {
        const bool input = side == "in";
        for (const auto &value : m_node[side].toArray()) {
            const auto port = value.toObject();
            const auto name = m_preview.portLabel(port);
            if (port["type"] == "adapter-ref") {
                body->addWidget(text(input ? "In    LoRA adapter · Optional" : "Out   LoRA adapter"));
                continue;
            }
            const auto type = contracts.value(port["type"].toString()).toObject()["label"].toString();
            const auto label = name == type ? name : QString("%1 · %2").arg(name, type);
            auto *edit = new QPushButton(QString((input ? "In    " : "Out   ") + label + "  ›").replace('&', "&&"));
            edit->setObjectName("blueprintPortContract");
            edit->setAccessibleName(QString("Edit %1 schema for %2").arg(input ? "input" : "output", name));
            edit->setCursor(Qt::PointingHandCursor);
            body->addWidget(edit);
            const auto portId = port["id"].toString();
            connect(edit, &QPushButton::clicked, this, [this, side, portId] { emit schemaRequested(side, portId); });
            for (const auto &edgeValue : edges) {
                const auto edge = edgeValue.toObject();
                if (input ? (edge["to"] != m_node["id"] || edge["in"] != port["id"])
                          : (edge["from"] != m_node["id"] || edge["out"] != port["id"]))
                    continue;
                const auto other = findById(nodes, (input ? edge["from"] : edge["to"]).toString());
                const auto otherPort = findById(other[input ? "out" : "in"].toArray(),
                                                (input ? edge["out"] : edge["in"]).toString());
                auto *row = new QHBoxLayout;
                row->setContentsMargins(12, 0, 0, 0);
                row->addWidget(text(QString(input ? "← %1 · %2" : "→ %1 · %2")
                    .arg(other["name"].toString(), m_preview.portLabel(otherPort))), 1);
                auto *disconnect = new Button("Disconnect");
                disconnect->setFlat(true);
                disconnect->setAccessibleName(QString("Disconnect %1 from %2").arg(other["name"].toString(), name));
                row->addWidget(disconnect);
                body->addLayout(row);
                connect(disconnect, &Button::clicked, this, [this, edge] { emit disconnectRequested(edge); });
            }
        }
    }
}

void BlueprintInspector::commitNodeFields() {
    if (!m_fields || m_fields->showError()) return;
    const auto node = m_fields->node();
    if (node == m_node) return;
    emit nodeChanged(node, m_fields->portsChanged());
}

void BlueprintInspector::commitBinding() {
    if (!m_binding) return;
    if (!m_binding->ready()) {
        m_bindingPending = true;
        return;
    }
    const auto error = m_binding->error();
    m_bindingError->setText(error);
    m_bindingError->setVisible(!error.isEmpty());
    if (!error.isEmpty() || m_binding->node() == m_node) return;
    emit nodeChanged(m_binding->node(), false);
}

void BlueprintInspector::commitDataset() {
    if (!m_dataset) return;
    const auto error = m_dataset->error();
    m_bindingError->setText(error);
    m_bindingError->setVisible(!error.isEmpty());
    if (!error.isEmpty() || m_dataset->node() == m_node) return;
    emit nodeChanged(m_dataset->node(), false);
}
