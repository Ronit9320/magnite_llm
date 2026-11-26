#include "panels/blueprintbindingfields.h"

#include "panels/blueprintpreview.h"
#include "widgets/combobox.h"
#include "widgets/spinbox.h"
#include "widgets/surface.h"

#include <QEvent>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPointer>
#include <QSignalBlocker>
#include <QVBoxLayout>
#include <functional>

namespace {
QLabel *note(const QString &text = {}, const char *style = "cardMeta") {
    auto *label = new QLabel(text);
    label->setTextFormat(Qt::PlainText);
    label->setObjectName(style);
    label->setWordWrap(true);
    label->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return label;
}

QJsonObject findById(const QJsonArray &array, const QString &id) {
    for (const auto &value : array)
        if (value.toObject()["id"].toString() == id) return value.toObject();
    return {};
}

void emitOnFocusOut(QPlainTextEdit *edit, QObject *watched, QEvent *event, const std::function<void()> &notify) {
    if (watched == edit && event->type() == QEvent::FocusOut && edit->document()->isModified()) {
        edit->document()->setModified(false);
        notify();
    }
}
}  // namespace

BlueprintBindingFields::BlueprintBindingFields(BlueprintPreview &preview, const QJsonObject &graph,
                                               const QJsonObject &node, QWidget *parent)
    : QWidget(parent), m_preview(preview), m_node(node), m_reasoning(node["kind"] == "reasoning"),
      m_linked(!BlueprintPreview::adapterSource(graph, node).isEmpty()) {
    setObjectName("blueprintBindingFields");
    auto *body = new QVBoxLayout(this);
    body->setContentsMargins(0, 0, 0, 0);
    m_models = new ComboBox;
    m_adapters = new ComboBox;
    m_note = note();
    if (!m_preview.live()) {
        const auto fixtures = m_preview.fixtures();
        for (const auto &value : fixtures["models"].toArray()) {
            const auto model = value.toObject();
            m_models->addItem(model["name"].toString() + " · " + model["engine"].toString(), model["id"].toString());
        }
        m_models->setCurrentIndex(qMax(0, m_models->findData(node["model"].toString())));
        Surface::field(body, "Base checkpoint", m_models);
        Surface::field(body, "Reuse a LoRA adapter", m_adapters);
        body->addWidget(m_note);
        refreshFixture();
        connect(m_models, &ComboBox::currentIndexChanged, this, [this] {
            refreshFixture();
            emit edited();
        });
        connect(m_adapters, &ComboBox::currentIndexChanged, this, &BlueprintBindingFields::edited);
        return;
    }

    for (auto *combo : {m_models, m_adapters}) {
        combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        combo->setMinimumContentsLength(22);
    }
    Surface::field(body, "Base checkpoint", m_models);
    Surface::field(body, "Direct LoRA adapter", m_adapters);
    body->addWidget(m_note);
    m_budget = new SpinBox;
    m_budget->setRange(1, 32768);
    m_budget->setValue(node["maxTokens"].toInt(m_reasoning ? 1024 : 256));
    Surface::field(body, "Maximum output tokens", m_budget);
    m_response = new ComboBox;
    m_response->addItem("Typed JSON", "json");
    m_response->addItem("Plain text", "text");
    m_response->setCurrentIndex(node["response"] == "text" ? 1 : 0);
    Surface::field(body, "Response format", m_response);
    m_prompt = new QPlainTextEdit(node["prompt"].toString("Return only the requested JSON value."));
    m_prompt->setTabChangesFocus(true);
    m_prompt->setFixedHeight(100);
    m_prompt->installEventFilter(this);
    Surface::field(body, "System prompt", m_prompt);
    loadLiveModels();
    m_preview.refreshOptions();
    connect(&m_preview, &BlueprintPreview::optionsChanged, this, &BlueprintBindingFields::loadLiveModels);
    connect(m_models, &ComboBox::currentIndexChanged, this, [this] {
        refreshLive();
        emit edited();
    });
    connect(m_adapters, &ComboBox::currentIndexChanged, this, &BlueprintBindingFields::edited);
    connect(m_budget, &SpinBox::editingFinished, this, &BlueprintBindingFields::edited);
    connect(m_response, &ComboBox::currentIndexChanged, this, &BlueprintBindingFields::edited);
}

bool BlueprintBindingFields::eventFilter(QObject *watched, QEvent *event) {
    emitOnFocusOut(m_prompt, watched, event, [this] { emit edited(); });
    return QWidget::eventFilter(watched, event);
}

void BlueprintBindingFields::setReady(bool ready) {
    if (m_ready == ready) return;
    m_ready = ready;
    emit readyChanged(ready);
}

void BlueprintBindingFields::refreshFixture() {
    const QSignalBlocker blocker(m_adapters);
    const auto model = findById(m_preview.fixtures()["models"].toArray(), m_models->currentData().toString());
    m_adapters->clear();
    m_adapters->addItem("Select compatible adapter");
    const bool supported = model["supported"].toBool();
    for (const auto &value : m_preview.compatibleAdapters(model["id"].toString(), BlueprintPreview::operationId(m_node))) {
        const auto adapter = value.toObject();
        m_adapters->addItem(adapter["name"].toString(), adapter["id"].toString());
    }
    m_adapters->setCurrentIndex(qMax(0, m_adapters->findData(m_node["adapter"].toString())));
    m_adapters->setEnabled(supported);
    m_note->setText(!supported ? "This engine cannot switch PEFT adapters between blueprint nodes."
        : m_adapters->count() == 1 ? "No adapters match this checkpoint and node type."
        : "Adapters must match the selected checkpoint and node type.");
}

void BlueprintBindingFields::loadLiveModels() {
    const auto selected = m_models->currentData().toString();
    {
        QSignalBlocker blocker(m_models);
        m_models->clear();
        m_models->addItem("Choose a checkpoint", "");
        for (const auto &value : m_preview.fixtures()["models"].toArray()) {
            const auto entry = value.toObject();
            const bool unsupported = m_reasoning && entry["thinking"] == false;
            m_models->addItem(entry["name"].toString() + (unsupported ? " · no thinking" : ""), entry["id"].toString());
            if (unsupported) BlueprintPreview::disableItem(m_models, m_models->count() - 1);
        }
        m_models->setCurrentIndex(qMax(0, m_models->findData(selected.isEmpty() ? m_node["model"].toString() : selected)));
    }
    refreshLive();
}

void BlueprintBindingFields::refreshLive() {
    const auto model = m_models->currentData().toString();
    {
        const QSignalBlocker blocker(m_adapters);
        m_adapters->clear();
        m_adapters->addItem("No adapter", "");
    }
    m_adapters->setEnabled(false);
    setReady(false);
    if (model.isEmpty()) {
        m_note->setText("Download a Transformers checkpoint in the model library, then select it here.");
        return;
    }
    m_note->setText("Checking this checkpoint…");
    m_preview.checkpoint(model, [guard = QPointer(this), model](const auto &info, const auto &error) {
        if (!guard || guard->m_models->currentData().toString() != model) return;
        if (!error.isEmpty()) { guard->m_note->setText(error); return; }
        const bool supported = info["reasoning"].toBool();
        if (guard->m_reasoning && !supported) {
            guard->m_note->setText("This checkpoint does not support explicit thinking.");
            return;
        }
        const int context = info["context_tokens"].toInt();
        guard->m_note->setText(QString("%1 context tokens%2").arg(context).arg(supported ? " · Explicit thinking available" : ""));
        guard->m_budget->setMaximum(qBound(1, context - 1, 32768));
        {
            const QSignalBlocker blocker(guard->m_adapters);
            guard->m_adapters->clear();
            guard->m_adapters->addItem("No adapter", "");
            for (const auto &value : info["adapters"].toArray()) {
                const auto adapter = value.toObject();
                guard->m_adapters->addItem(adapter["name"].toString(), adapter["id"].toString());
            }
            guard->m_adapters->setCurrentIndex(qMax(0, guard->m_adapters->findData(guard->m_node["adapter"].toString())));
        }
        guard->m_adapters->setEnabled(!guard->m_linked);
        guard->setReady(true);
    });
}

QJsonObject BlueprintBindingFields::node() const {
    auto node = m_node;
    node["model"] = m_models->currentData().toString();
    node["adapter"] = m_adapters->currentData().toString();
    if (m_preview.live()) {
        node["maxTokens"] = m_budget->value();
        node["response"] = m_response->currentData().toString();
        node["prompt"] = m_prompt->toPlainText();
    }
    return node;
}

QString BlueprintBindingFields::error() const {
    if (!m_preview.live()) {
        if (!m_adapters->isEnabled() || m_adapters->currentIndex() < 1)
            return "Choose a Torch / PEFT checkpoint and a compatible adapter.";
        return {};
    }
    if (m_prompt->toPlainText().size() > 16000) return "Use at most 16,000 characters for the system prompt.";
    if (m_response->currentData() == "text") {
        const auto ports = m_node["out"].toArray();
        if (ports.size() != 1 || ports.first().toObject()["schema"].toObject()["type"] != "string")
            return "Plain text requires one string output port.";
    }
    return {};
}

BlueprintDatasetFields::BlueprintDatasetFields(BlueprintPreview &preview, const QJsonObject &node, QWidget *parent)
    : QWidget(parent), m_preview(preview), m_node(node) {
    setObjectName("blueprintDatasetFields");
    auto *body = new QVBoxLayout(this);
    body->setContentsMargins(0, 0, 0, 0);
    m_datasets = new ComboBox;
    m_datasets->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_datasets->setMinimumContentsLength(22);
    populate();
    connect(&m_preview, &BlueprintPreview::optionsChanged, this, &BlueprintDatasetFields::populate);
    m_preview.refreshOptions();
    Surface::field(body, "Source", m_datasets);
    m_rows = new QPlainTextEdit(node["setting"].toString("[]"));
    m_rows->setTabChangesFocus(true);
    m_rows->installEventFilter(this);
    auto *field = new QWidget;
    auto *rowsBody = new QVBoxLayout(field);
    rowsBody->setContentsMargins(0, 0, 0, 0);
    Surface::field(rowsBody, "Inline rows (JSON array)", m_rows);
    body->addWidget(field);
    const auto enabled = [this, field] { field->setVisible(m_datasets->currentData().toString().isEmpty()); };
    connect(m_datasets, &ComboBox::currentIndexChanged, this, [this, enabled] {
        enabled();
        emit edited();
    });
    enabled();
    body->addWidget(note("Add local datasets in Training. Set the row type under the Rows port’s JSON Schema."));
}

bool BlueprintDatasetFields::eventFilter(QObject *watched, QEvent *event) {
    emitOnFocusOut(m_rows, watched, event, [this] { emit edited(); });
    return QWidget::eventFilter(watched, event);
}

void BlueprintDatasetFields::populate() {
    const QSignalBlocker blocker(m_datasets);
    const auto current = m_datasets->count() ? m_datasets->currentData().toString() : m_node["dataset"].toString();
    m_datasets->clear();
    m_datasets->addItem("Inline rows", "");
    for (const auto &value : m_preview.datasets()) {
        const auto item = value.toObject();
        m_datasets->addItem(QString("%1 · %2 rows").arg(item["name"].toString()).arg(item["rows"].toInt()), item["id"].toString());
    }
    if (!current.isEmpty() && m_datasets->findData(current) < 0) m_datasets->addItem("Missing dataset · " + current, current);
    m_datasets->setCurrentIndex(qMax(0, m_datasets->findData(current)));
}

QJsonObject BlueprintDatasetFields::node() const {
    auto node = m_node;
    node["dataset"] = m_datasets->currentData().toString();
    node["setting"] = m_rows->toPlainText();
    return node;
}

QString BlueprintDatasetFields::error() const { return m_preview.settingError(node()); }
