#include "panels/blueprintlibrarydock.h"

#include "panels/blueprintpreview.h"
#include "theme.h"
#include "widgets/blueprintstyle.h"
#include "widgets/button.h"
#include "widgets/edgescrollbar.h"
#include "widgets/flowlayout.h"

#include <QApplication>
#include <QButtonGroup>
#include <QDrag>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QMouseEvent>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QVBoxLayout>
#include <functional>

namespace {
const QStringList kExampleOperations{"selector", "extractor", "lookup", "pricing"};

QLabel *text(const QString &value, const char *style, bool wrap = false) {
    auto *label = new QLabel(value);
    label->setTextFormat(Qt::PlainText);
    label->setObjectName(style);
    label->setWordWrap(wrap);
    label->setAttribute(Qt::WA_TransparentForMouseEvents);
    return label;
}

Button *smallButton(const QString &icon, const QString &name) {
    auto *button = new Button({});
    button->setFlat(true);
    button->setObjectName("blueprintIconButton");
    button->setAccessibleName(name);
    button->setToolTip(name);
    button->setFixedSize(24, 24);
    button->setProperty("themeIcon", icon);
    button->setIcon(Theme::coloredIcon(icon, QColor(Theme::textSecondary()), 14));
    return button;
}

// A library row: click or Enter adds the entry, dragging carries it onto the canvas.
class LibraryRow : public QFrame {
public:
    LibraryRow(const QByteArray &payload, std::function<void()> activate)
        : m_payload(payload), m_activate(std::move(activate)) {
        setObjectName("blueprintLibraryRow");
        setFocusPolicy(Qt::StrongFocus);
        setCursor(Qt::OpenHandCursor);
    }

protected:
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton) m_press = event->position().toPoint();
        event->accept();
    }
    void mouseMoveEvent(QMouseEvent *event) override {
        if (m_press.isNull() || !(event->buttons() & Qt::LeftButton)) return;
        if ((event->position().toPoint() - m_press).manhattanLength() < QApplication::startDragDistance()) return;
        m_press = {};
        auto *drag = new QDrag(this);
        auto *mime = new QMimeData;
        mime->setData(BlueprintStyle::entryMimeType, m_payload);
        drag->setMimeData(mime);
        drag->setPixmap(grab());
        drag->exec(Qt::CopyAction);
    }
    void mouseReleaseEvent(QMouseEvent *event) override {
        const bool clicked = !m_press.isNull() && rect().contains(event->position().toPoint());
        m_press = {};
        if (clicked) m_activate();
    }
    void keyPressEvent(QKeyEvent *event) override {
        if (event->modifiers() == Qt::NoModifier
            && (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter || event->key() == Qt::Key_Space)) {
            m_activate();
            return;
        }
        QFrame::keyPressEvent(event);
    }

private:
    QByteArray m_payload;
    std::function<void()> m_activate;
    QPoint m_press;
};
}  // namespace

QByteArray BlueprintLibraryDock::Entry::toJson() const {
    QJsonObject object;
    if (!operation.isEmpty()) object["operation"] = operation;
    else {
        object["savedNode"] = savedNode;
        object["savedId"] = savedId;
        object["savedName"] = savedName;
    }
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

BlueprintLibraryDock::Entry BlueprintLibraryDock::Entry::fromJson(const QByteArray &json) {
    const auto object = QJsonDocument::fromJson(json).object();
    return {object["operation"].toString(), object["savedNode"].toObject(), object["savedId"].toString(),
            object["savedName"].toString()};
}

QList<QPair<QString, QList<BlueprintLibraryDock::Entry>>> BlueprintLibraryDock::groups(
    const BlueprintPreview &preview, const QString &query, const QString &family) {
    const auto matches = [&](const QString &label, const QString &detail) {
        return query.isEmpty() || label.contains(query, Qt::CaseInsensitive) || detail.contains(query, Qt::CaseInsensitive);
    };
    QHash<QString, QList<Entry>> byFamily;
    for (const auto &value : preview.fixtures()["operations"].toArray()) {
        const auto spec = value.toObject();
        const auto id = spec["id"].toString();
        if (spec["legacyOnly"].toBool() || kExampleOperations.contains(id)) continue;
        const auto group = BlueprintStyle::nodeFamily(spec["kind"].toString());
        if (!family.isEmpty() && family != group) continue;
        if (!matches(spec["label"].toString(), spec["detail"].toString())) continue;
        byFamily[group].append({id, {}, {}, {}});
    }
    QList<QPair<QString, QList<Entry>>> result;
    for (const auto &group : BlueprintStyle::families())
        if (!byFamily.value(group).isEmpty()) result.append({group, byFamily.value(group)});
    if (family.isEmpty() || family == savedGroup) {
        QList<Entry> saved;
        for (const auto &value : preview.nodeLibrary()) {
            const auto entry = value.toObject();
            const auto node = entry["node"].toObject();
            const auto spec = preview.operation(BlueprintPreview::operationId(node));
            if (!matches(entry["name"].toString(), spec["label"].toString())) continue;
            saved.append({{}, node, entry["id"].toString(), entry["name"].toString()});
        }
        if (!saved.isEmpty()) result.append({savedGroup, saved});
    }
    return result;
}

QString BlueprintLibraryDock::title(const BlueprintPreview &preview, const Entry &entry) {
    return entry.operation.isEmpty() ? entry.savedName : preview.operation(entry.operation)["label"].toString();
}

BlueprintLibraryDock::BlueprintLibraryDock(BlueprintPreview &preview, QWidget *parent)
    : QWidget(parent), m_preview(preview) {
    setObjectName("blueprintLibrary");
    auto *body = new QVBoxLayout(this);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(8);

    auto *tabRow = new QHBoxLayout;
    tabRow->setSpacing(4);
    m_tabs = new QButtonGroup(this);
    for (const auto &name : {QString("Library"), QString("In this workflow")}) {
        auto *tab = new Button(name);
        tab->setObjectName("blueprintSegment");
        tab->setCheckable(true);
        tab->setAccessibleName(name);
        m_tabs->addButton(tab, m_tabs->buttons().size());
        tabRow->addWidget(tab);
    }
    tabRow->addStretch();
    m_tabs->button(0)->setChecked(true);
    body->addLayout(tabRow);

    m_pages = new QStackedWidget;
    body->addWidget(m_pages, 1);
    connect(m_tabs, &QButtonGroup::idClicked, m_pages, &QStackedWidget::setCurrentIndex);

    auto *library = new QWidget;
    auto *libraryBody = new QVBoxLayout(library);
    libraryBody->setContentsMargins(0, 0, 0, 0);
    libraryBody->setSpacing(8);
    m_search = new QLineEdit;
    m_search->setPlaceholderText("Search nodes  /");
    m_search->setAccessibleName("Search nodes");
    m_search->setClearButtonEnabled(true);
    libraryBody->addWidget(m_search);
    auto *chips = new QWidget;
    // FlowLayout lays chips out as an even grid; three columns fit the dock at every size.
    auto *chipFlow = new FlowLayout(chips, 4, 4);
    chipFlow->setContentsMargins(0, 0, 0, 0);
    chipFlow->setColumnMinimumWidth(64);
    chipFlow->setMaximumColumns(3);
    m_chips = new QButtonGroup(this);
    QStringList chipNames{QString()};
    chipNames << BlueprintStyle::families() << savedGroup;
    for (const auto &family : chipNames) {
        const auto label = family.isEmpty() ? QString("All")
            : family == savedGroup ? QString(savedGroup) : BlueprintStyle::familyLabel(family);
        auto *chip = new Button(label);
        chip->setObjectName("blueprintLibraryChip");
        chip->setCheckable(true);
        chip->setProperty("family", family);
        chip->setAccessibleName("Show " + label);
        m_chips->addButton(chip);
        chipFlow->addWidget(chip);
    }
    m_chips->buttons().first()->setChecked(true);
    libraryBody->addWidget(chips);
    connect(m_chips, &QButtonGroup::buttonClicked, this, [this](QAbstractButton *chip) {
        m_family = chip->property("family").toString();
        refreshLibrary();
    });
    connect(m_search, &QLineEdit::textChanged, this, &BlueprintLibraryDock::refreshLibrary);

    auto *scroll = new QScrollArea;
    scroll->setObjectName("blueprintLibraryScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *rows = new QWidget;
    m_rows = new QVBoxLayout(rows);
    m_rows->setContentsMargins(0, 0, 6, 0);
    m_rows->setSpacing(2);
    scroll->setWidget(rows);
    EdgeScrollBar::attach(scroll);
    libraryBody->addWidget(scroll, 1);
    m_pages->addWidget(library);

    m_workflow = new QListWidget;
    m_workflow->setObjectName("blueprintWorkflowList");
    m_workflow->setAccessibleName("Workflow nodes");
    m_workflow->setFrameShape(QFrame::NoFrame);
    m_workflow->setIconSize(QSize(15, 15));
    m_pages->addWidget(m_workflow);
    connect(m_workflow, &QListWidget::currentItemChanged, this, [this](QListWidgetItem *item) {
        if (item) emit nodeActivated(item->data(Qt::UserRole).toString());
    });
    connect(&m_preview, &BlueprintPreview::nodeLibraryChanged, this, &BlueprintLibraryDock::refreshLibrary);
    refreshLibrary();
}

QWidget *BlueprintLibraryDock::row(const Entry &entry) {
    const auto spec = m_preview.operation(entry.operation.isEmpty()
        ? BlueprintPreview::operationId(entry.savedNode) : entry.operation);
    const auto kind = spec["kind"].toString(entry.savedNode["kind"].toString());
    const auto name = title(m_preview, entry);
    auto *frame = new LibraryRow(entry.toJson(), [this, entry] { emit addRequested(entry); });
    frame->setAccessibleName("Add " + name);
    frame->setToolTip(spec["detail"].toString());
    auto *layout = new QHBoxLayout(frame);
    layout->setContentsMargins(8, 6, 4, 6);
    layout->setSpacing(8);
    auto *icon = text({}, "blueprintLibraryIcon");
    icon->setPixmap(Theme::coloredIcon(BlueprintStyle::nodeIcon(kind),
        BlueprintStyle::familyColor(BlueprintStyle::nodeFamily(kind)), 16).pixmap(16, 16));
    icon->setFixedSize(20, 20);
    layout->addWidget(icon, 0, Qt::AlignTop);
    auto *labels = new QVBoxLayout;
    labels->setSpacing(1);
    // Both lines give way to the dock's width; the row's tooltip carries the full description.
    auto *titleLabel = text(name, "blueprintLibraryTitle");
    titleLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    labels->addWidget(titleLabel);
    const auto detail = entry.operation.isEmpty() ? spec["label"].toString() : spec["detail"].toString();
    auto *detailLabel = text({}, "cardMeta");
    detailLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    detailLabel->setText(detailLabel->fontMetrics().elidedText(detail, Qt::ElideRight, 160));
    labels->addWidget(detailLabel);
    layout->addLayout(labels, 1);
    auto *details = smallButton("info", "Details for " + name);
    layout->addWidget(details, 0, Qt::AlignTop);
    connect(details, &Button::clicked, this, [this, entry] { emit detailsRequested(entry); });
    if (!entry.savedId.isEmpty()) {
        auto *remove = smallButton("trash", "Remove from node library");
        layout->addWidget(remove, 0, Qt::AlignTop);
        connect(remove, &Button::clicked, this, [this, id = entry.savedId] { m_preview.removeNodeTemplate(id); });
    }
    return frame;
}

void BlueprintLibraryDock::refreshLibrary() {
    while (auto *item = m_rows->takeAt(0)) {
        if (auto *widget = item->widget()) widget->deleteLater();
        delete item;
    }
    const auto found = groups(m_preview, m_search->text().trimmed(), m_family);
    for (const auto &[group, entries] : found) {
        auto *header = text(group == savedGroup ? QString(savedGroup) : BlueprintStyle::familyLabel(group),
                            "blueprintLibraryGroup");
        m_rows->addWidget(header);
        for (const auto &entry : entries) m_rows->addWidget(row(entry));
    }
    if (found.isEmpty()) m_rows->addWidget(text("No nodes match.", "cardMeta", true));
    m_rows->addStretch();
}

void BlueprintLibraryDock::setGraph(const QJsonObject &graph, const QHash<QString, QString> &warnings) {
    const QSignalBlocker blocker(m_workflow);
    const auto current = m_workflow->currentItem() ? m_workflow->currentItem()->data(Qt::UserRole).toString() : QString{};
    m_workflow->clear();
    for (const auto &value : graph["nodes"].toArray()) {
        const auto node = value.toObject();
        const auto spec = m_preview.operation(BlueprintPreview::operationId(node));
        const auto kind = spec["kind"].toString(node["kind"].toString());
        const auto warning = warnings.value(node["id"].toString());
        auto label = node["name"].toString() + "\n" + spec["label"].toString();
        if (!warning.isEmpty()) label += " · needs input";
        auto *item = new QListWidgetItem(Theme::coloredIcon(BlueprintStyle::nodeIcon(kind),
            BlueprintStyle::familyColor(BlueprintStyle::nodeFamily(kind)), 15), label, m_workflow);
        item->setData(Qt::UserRole, node["id"].toString());
        item->setToolTip(warning);
        if (node["id"].toString() == current) m_workflow->setCurrentItem(item);
    }
}

void BlueprintLibraryDock::setCurrentNode(const QString &id) {
    const QSignalBlocker blocker(m_workflow);
    m_workflow->setCurrentItem(nullptr);
    for (int i = 0; i < m_workflow->count(); ++i)
        if (m_workflow->item(i)->data(Qt::UserRole).toString() == id) m_workflow->setCurrentRow(i);
}

void BlueprintLibraryDock::focusSearch() {
    m_tabs->button(0)->click();
    m_search->setFocus();
    m_search->selectAll();
}
