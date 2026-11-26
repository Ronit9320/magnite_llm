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
    m_root->setContentsMargins(10, 8, 10, 10);
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
        // Init-captures: older Apple clang cannot capture structured bindings directly.
        QTimer::singleShot(0, this, [this, node = node, ports = ports] { emit nodeChanged(node, ports); });
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

    const auto iconKind = code == "selector" || code == "extractor" ? code : kind;
    const QColor family = BlueprintStyle::familyColor(BlueprintStyle::nodeFamily(iconKind));
    auto *header = new QHBoxLayout;
    header->setSpacing(10);
    auto *icon = text({}, "blueprintInspectorGlyph", false);
    icon->setFixedSize(36, 36);
    icon->setAlignment(Qt::AlignCenter);
    QColor tint = family;
    tint.setAlphaF(0.16);
    icon->setStyleSheet(QString("background: rgba(%1, %2, %3, %4); border-radius: 10px;")
        .arg(tint.red()).arg(tint.green()).arg(tint.blue()).arg(tint.alphaF(), 0, 'f', 2));
    icon->setPixmap(Theme::coloredIcon(BlueprintStyle::nodeIcon(iconKind), family, 18).pixmap(18, 18));
    header->addWidget(icon);
    auto *names = new QVBoxLayout;
    names->setSpacing(1);
    names->addWidget(text(m_node["name"].toString(), "blueprintInspectorTitle"));
    names->addWidget(text(spec["label"].toString(m_preview.nodeKindLabel(kind)), "blueprintInspectorSubtitle"));
    header->addLayout(names, 1);
    auto *details = iconButton("info", "Node details");
    auto *close = iconButton("window-close", "Clear node selection");
    header->addWidget(details, 0, Qt::AlignTop);
    header->addWidget(close, 0, Qt::AlignTop);
    connect(details, &Button::clicked, this, [this, id] { emit detailsRequested(id); });
    connect(close, &Button::clicked, this, &BlueprintInspector::closeRequested);
    body->addLayout(header);
    body->addSpacing(6);

    const auto warnings = BlueprintScene::warningLines(m_preview, m_graph, m_node, m_preview.issues(m_graph));
    if (!warnings.isEmpty()) {
        auto *warning = text(warnings.join('\n'), "blueprintWarning");
        warning->setAccessibleName("Node warnings");
        body->addWidget(warning);
    }

    const bool liveModel = m_preview.live() && (kind == "model" || kind == "reasoning");
    m_fields = new BlueprintNodeFields(m_preview, kind, m_node);
    m_fields->setHelpShown(false);
    if (liveModel) m_fields->setBindingShown(false);
    body->addWidget(m_fields);
    connect(m_fields, &BlueprintNodeFields::edited, this, &BlueprintInspector::commitNodeFields);

    if (liveModel || (!m_preview.live() && kind == "model")) {
        body->addSpacing(6);
        body->addWidget(text(kind == "reasoning" ? "Reasoning checkpoint" : "Model & adapter", "blueprintInspectorSection", false));
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
        body->addSpacing(6);
        body->addWidget(text("Dataset", "blueprintInspectorSection", false));
        m_dataset = new BlueprintDatasetFields(m_preview, m_node);
        body->addWidget(m_dataset);
        m_bindingError = text({}, "blueprintError");
        m_bindingError->hide();
        body->addWidget(m_bindingError);
        connect(m_dataset, &BlueprintDatasetFields::edited, this, &BlueprintInspector::commitDataset);
    }

    body->addSpacing(6);
    addPorts(body);
    body->addSpacing(8);
    auto *actions = new QHBoxLayout;
    actions->setSpacing(8);
    auto *test = new Button("Test this node");
    auto *save = iconButton("folder", "Save to node library");
    auto *remove = new Button("Delete");
    remove->setObjectName("destructiveBtn");
    remove->setAccessibleName("Delete node");
    remove->setToolTip("Delete node");
    actions->addWidget(test, 1);
    actions->addWidget(save);
    actions->addWidget(remove);
    body->addLayout(actions);
    connect(test, &Button::clicked, this, [this, id] { emit testRequested(id); });
    connect(save, &Button::clicked, this, [this, id] { emit saveToLibraryRequested(id); });
    connect(remove, &Button::clicked, this, [this, id] { emit deleteRequested(id); });
    body->addStretch();
    // Long checkpoint or operation names stay in the popup; the closed box fits the panel.
    for (auto *combo : m_content->findChildren<QComboBox *>()) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(qMin(combo->minimumContentsLength() > 0 ? combo->minimumContentsLength() : 10, 10));
    }
    restoreFocus(m_content, spot);
}

void BlueprintInspector::addPorts(QVBoxLayout *body) {
    const auto contracts = m_preview.fixtures()["contracts"].toObject();
    const auto nodes = m_graph["nodes"].toArray();
    const auto edges = m_graph["edges"].toArray();
    for (const auto &side : {QString("in"), QString("out")}) {
        const bool input = side == "in";
        const auto ports = m_node[side].toArray();
        if (ports.isEmpty()) continue;
        body->addWidget(text(input ? "Inputs" : "Outputs", "blueprintInspectorSection", false));
        for (const auto &value : ports) {
            const auto port = value.toObject();
            const auto name = m_preview.portLabel(port);
            const bool adapter = port["type"] == "adapter-ref";
            // One rounded row per pin: type dot, name and type; below it, each wire with its own unlink.
            auto *row = new QFrame;
            row->setObjectName("blueprintPortRow");
            auto *rowLayout = new QVBoxLayout(row);
            rowLayout->setContentsMargins(0, 0, 0, 0);
            rowLayout->setSpacing(0);
            auto *head = new QPushButton;
            head->setObjectName("blueprintPortContract");
            auto *headLayout = new QHBoxLayout(head);
            headLayout->setContentsMargins(10, 0, 10, 0);
            headLayout->setSpacing(8);
            auto *dot = text({}, "blueprintPortDot", false);
            dot->setFixedSize(8, 8);
            dot->setStyleSheet(QString("background: %1; border-radius: 4px;")
                .arg(BlueprintStyle::typeColor(port["type"].toString()).name()));
            headLayout->addWidget(dot);
            auto *nameLabel = text(adapter ? QString("LoRA adapter") : name, "blueprintPortName", false);
            nameLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
            headLayout->addWidget(nameLabel, 1);
            const auto type = adapter ? QString(input ? "adapter · optional" : "adapter")
                                      : contracts.value(port["type"].toString()).toObject()["label"].toString().toLower();
            headLayout->addWidget(text(type, "blueprintPortType", false));
            for (auto *label : head->findChildren<QLabel *>()) label->setAttribute(Qt::WA_TransparentForMouseEvents);
            if (adapter) {
                head->setEnabled(false);
                head->setFocusPolicy(Qt::NoFocus);
            } else {
                head->setAccessibleName(QString("Edit %1 schema for %2").arg(input ? "input" : "output", name));
                head->setToolTip(QString("Edit the %1 schema").arg(input ? "input" : "output"));
                head->setCursor(Qt::PointingHandCursor);
                const auto portId = port["id"].toString();
                connect(head, &QPushButton::clicked, this, [this, side, portId] { emit schemaRequested(side, portId); });
            }
            rowLayout->addWidget(head);
            int wires = 0;
            for (const auto &edgeValue : edges) {
                const auto edge = edgeValue.toObject();
                if (input ? (edge["to"] != m_node["id"] || edge["in"] != port["id"])
                          : (edge["from"] != m_node["id"] || edge["out"] != port["id"]))
                    continue;
                ++wires;
                const auto other = findById(nodes, (input ? edge["from"] : edge["to"]).toString());
                const auto otherPort = findById(other[input ? "out" : "in"].toArray(),
                                                (input ? edge["out"] : edge["in"]).toString());
                auto *wire = new QHBoxLayout;
                wire->setContentsMargins(26, 0, 4, 4);
                wire->setSpacing(4);
                auto *label = text(QString(input ? "← %1 · %2" : "→ %1 · %2")
                    .arg(other["name"].toString(), m_preview.portLabel(otherPort)), "blueprintPortWire", false);
                label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
                label->setToolTip(label->text());
                wire->addWidget(label, 1);
                auto *disconnect = iconButton("window-close", QString("Disconnect %1 from %2").arg(other["name"].toString(), name));
                disconnect->setFixedSize(22, 22);
                disconnect->setIcon(Theme::coloredIcon("window-close", QColor(Theme::textMuted()), 12));
                wire->addWidget(disconnect);
                rowLayout->addLayout(wire);
                connect(disconnect, &Button::clicked, this, [this, edge] { emit disconnectRequested(edge); });
            }
            if (input && !adapter && !wires) {
                auto *missing = text("Not connected", "blueprintPortMissing", false);
                missing->setContentsMargins(26, 0, 0, 6);
                rowLayout->addWidget(missing);
            }
            body->addWidget(row);
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
