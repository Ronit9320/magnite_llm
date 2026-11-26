#include "panels/blueprintdetails.h"

#include "panels/blueprintpreview.h"
#include "theme.h"
#include "widgets/blueprintstyle.h"
#include "widgets/blueprintview.h"
#include "widgets/button.h"

#include <QButtonGroup>
#include <QFile>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLabel>
#include <QMouseEvent>
#include <QLinearGradient>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QShortcut>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace {
constexpr int kWidth = 380;

QLabel *text(const QString &value, const char *style = "cardMeta") {
    auto *label = new QLabel(value);
    label->setTextFormat(Qt::PlainText);
    label->setObjectName(style);
    label->setWordWrap(true);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return label;
}

void clear(QVBoxLayout *layout) {
    while (auto *item = layout->takeAt(0)) {
        if (auto *widget = item->widget()) widget->deleteLater();
        else if (auto *inner = item->layout()) {
            while (auto *child = inner->takeAt(0)) {
                if (auto *widget = child->widget()) widget->deleteLater();
                delete child;
            }
        }
        delete item;
    }
}

QJsonObject findById(const QJsonArray &array, const QString &id) {
    for (const auto &value : array)
        if (value.toObject()["id"].toString() == id) return value.toObject();
    return {};
}

QString rgba(QColor color, qreal alpha) {
    color.setAlphaF(alpha);
    return QString("rgba(%1, %2, %3, %4)").arg(color.red()).arg(color.green()).arg(color.blue()).arg(alpha, 0, 'f', 2);
}

// The window's header: the node family's colour washed down from the top edge.
class DetailsHeader : public QWidget {
public:
    QColor tint;

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QLinearGradient wash(0, 0, 0, height());
        QColor top = tint;
        top.setAlphaF(0.18);
        QColor bottom = tint;
        bottom.setAlphaF(0);
        wash.setColorAt(0, top);
        wash.setColorAt(1, bottom);
        painter.setPen(Qt::NoPen);
        painter.setBrush(wash);
        painter.drawRoundedRect(rect(), 9, 9);
    }
};

// A small-capitals section title.
QLabel *section(const QString &title) {
    auto *label = new QLabel(title.toUpper());
    label->setObjectName("blueprintDetailsSection");
    return label;
}

// A dark inset block: a small caption over monospaced content.
QFrame *block(const QString &caption, const QString &content) {
    auto *frame = new QFrame;
    frame->setObjectName("blueprintDetailsBlock");
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(10, 8, 10, 8);
    layout->setSpacing(3);
    layout->addWidget(text(caption, "blueprintDetailsCaption"));
    layout->addWidget(text(content, "blueprintDetailsCode"));
    return frame;
}
}  // namespace

QJsonObject BlueprintDetails::docs() {
    static const QJsonObject loaded = [] {
        QFile file(":/blueprints/node-docs.json");
        return file.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(file.readAll()).object() : QJsonObject{};
    }();
    return loaded;
}

BlueprintDetails::BlueprintDetails(const BlueprintPreview &preview, QWidget *parent)
    : FloatingPanel(parent), m_preview(preview) {
    setObjectName("blueprintDetails");
    setAccessibleName("Node details");
    auto *body = new QVBoxLayout(this);
    body->setSpacing(6);
    m_header = new DetailsHeader;
    m_header->setCursor(Qt::SizeAllCursor);
    auto *header = new QHBoxLayout(m_header);
    header->setContentsMargins(10, 10, 6, 10);
    header->setSpacing(12);
    m_glyph = new QLabel;
    m_glyph->setFixedSize(38, 38);
    m_glyph->setAlignment(Qt::AlignCenter);
    header->addWidget(m_glyph);
    auto *names = new QVBoxLayout;
    names->setSpacing(1);
    m_title = text({}, "blueprintInspectorTitle");
    m_subtitle = text({}, "blueprintInspectorSubtitle");
    names->addWidget(m_title);
    names->addWidget(m_subtitle);
    header->addLayout(names, 1);
    auto *close = new Button({});
    close->setFlat(true);
    close->setObjectName("blueprintIconButton");
    close->setAccessibleName("Close node details");
    close->setToolTip("Close (Esc)");
    close->setFixedSize(30, 30);
    close->setProperty("themeIcon", "window-close");
    close->setIcon(Theme::coloredIcon("window-close", QColor(Theme::textSecondary()), 14));
    header->addWidget(close, 0, Qt::AlignTop);
    connect(close, &Button::clicked, this, &QWidget::hide);
    body->addWidget(m_header);

    auto *tabs = new QHBoxLayout;
    tabs->setContentsMargins(4, 0, 4, 0);
    tabs->setSpacing(2);
    m_tabs = new QButtonGroup(this);
    for (const auto &name : {QString("Overview"), QString("Inputs & outputs"), QString("Last run")}) {
        auto *tab = new QPushButton(QString(name).replace('&', "&&"));
        tab->setObjectName("blueprintDetailsTab");
        tab->setCheckable(true);
        tab->setAccessibleName(name);
        tab->setCursor(Qt::PointingHandCursor);
        m_tabs->addButton(tab, m_tabs->buttons().size());
        tabs->addWidget(tab);
        if (name == "Last run") m_lastRunTab = tab;
    }
    tabs->addStretch();
    body->addLayout(tabs);
    m_pages = new QStackedWidget;
    for (auto **layout : {&m_overview, &m_ports, &m_lastRun}) {
        auto *page = new QWidget;
        *layout = new QVBoxLayout(page);
        (*layout)->setContentsMargins(10, 4, 10, 8);
        (*layout)->setSpacing(6);
        m_pages->addWidget(page);
    }
    // Long explanations scroll inside the window instead of squeezing.
    auto *scroll = new QScrollArea;
    scroll->setObjectName("blueprintDetailsScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(m_pages);
    body->addWidget(scroll, 1);

    auto *foot = new QFrame;
    foot->setObjectName("blueprintQuickAddFoot");
    m_footer = new QHBoxLayout(foot);
    m_footer->setContentsMargins(8, 8, 8, 8);
    m_footer->setSpacing(8);
    body->addWidget(foot);

    connect(m_tabs, &QButtonGroup::idClicked, m_pages, &QStackedWidget::setCurrentIndex);
    auto *escape = new QShortcut(QKeySequence(Qt::Key_Escape), this, nullptr, nullptr, Qt::WidgetWithChildrenShortcut);
    connect(escape, &QShortcut::activated, this, &QWidget::hide);
    hide();
}

void BlueprintDetails::showFor(const QString &operation, const QJsonObject &node, const QJsonObject &graph,
                               const QJsonObject &lastStep, const QRect &anchor) {
    const auto spec = m_preview.operation(operation);
    const auto doc = docs().value(operation).toObject();
    const auto kind = spec["kind"].toString(node["kind"].toString());
    const auto family = BlueprintStyle::nodeFamily(kind);
    const QColor tone = BlueprintStyle::familyColor(family);
    m_nodeId = node["id"].toString();
    static_cast<DetailsHeader *>(m_header)->tint = tone;
    m_header->update();
    m_glyph->setStyleSheet(QString("background: %1; border-radius: 11px;").arg(rgba(tone, 0.18)));
    m_glyph->setPixmap(Theme::coloredIcon(BlueprintStyle::nodeIcon(kind), tone, 18).pixmap(18, 18));
    m_title->setText(node.isEmpty() ? spec["label"].toString(operation) : node["name"].toString());
    m_subtitle->setText((node.isEmpty() || spec.isEmpty() ? QString() : spec["label"].toString() + " · ")
                        + BlueprintStyle::familyLabel(family));

    clear(m_overview);
    const auto about = doc["about"].toString(spec["detail"].toString());
    m_overview->addWidget(text(about.isEmpty() ? "No description is available for this node yet." : about,
                               "blueprintDetailsBody"));
    const auto tips = doc["tips"].toArray();
    if (!tips.isEmpty()) {
        m_overview->addSpacing(4);
        m_overview->addWidget(section("Good to know"));
        for (const auto &tip : tips) m_overview->addWidget(text("•  " + tip.toString(), "blueprintDetailsBody"));
    }
    m_overview->addStretch();

    clear(m_ports);
    const auto notes = doc["ports"].toObject();
    const auto contracts = m_preview.fixtures()["contracts"].toObject();
    const auto nodes = graph["nodes"].toArray();
    const auto edges = graph["edges"].toArray();
    const auto source = node.isEmpty() ? spec : node;
    for (const auto &side : {QString("in"), QString("out")}) {
        const bool input = side == "in";
        const auto ports = source[side].toArray();
        m_ports->addWidget(section(input ? "Inputs" : "Outputs"));
        if (ports.isEmpty())
            m_ports->addWidget(text(input ? "None. This node starts a branch of the workflow."
                                          : "None. Its value appears in the run result.", "blueprintDetailsBody"));
        for (const auto &value : ports) {
            const auto port = value.toObject();
            const auto type = port["type"].toString();
            const bool adapter = type == "adapter-ref";
            const QColor typeTone = BlueprintStyle::typeColor(type);
            auto *card = new QFrame;
            card->setObjectName("blueprintDetailsPort");
            auto *rows = new QVBoxLayout(card);
            rows->setContentsMargins(10, 8, 10, 8);
            rows->setSpacing(3);
            auto *head = new QHBoxLayout;
            head->setSpacing(8);
            auto *dot = new QLabel;
            dot->setFixedSize(8, 8);
            dot->setStyleSheet(QString("background: %1; border-radius: 4px;").arg(typeTone.name()));
            head->addWidget(dot);
            head->addWidget(text(adapter ? QString("LoRA adapter") : m_preview.portLabel(port), "blueprintDetailsPortName"), 1);
            auto *typeLabel = text(adapter ? QString(input ? "adapter · optional" : "adapter")
                                           : contracts.value(type).toObject()["label"].toString(type).toLower(),
                                   "blueprintPortType");
            typeLabel->setWordWrap(false);
            typeLabel->setStyleSheet(QString("color: %1;").arg(typeTone.name()));
            head->addWidget(typeLabel);
            rows->addLayout(head);
            const auto note = notes[side + "." + port["id"].toString()].toString();
            if (!note.isEmpty()) {
                auto *noteLabel = text(note, "blueprintDetailsNote");
                noteLabel->setContentsMargins(16, 0, 0, 0);
                rows->addWidget(noteLabel);
            }
            if (!node.isEmpty()) {
                QStringList wired;
                for (const auto &edgeValue : edges) {
                    const auto edge = edgeValue.toObject();
                    if (input ? (edge["to"] != node["id"] || edge["in"] != port["id"])
                              : (edge["from"] != node["id"] || edge["out"] != port["id"]))
                        continue;
                    wired << findById(nodes, (input ? edge["from"] : edge["to"]).toString())["name"].toString();
                }
                QLabel *line = nullptr;
                if (!wired.isEmpty())
                    line = text(QString(input ? "← Wired from %1" : "→ Wired to %1").arg(wired.join(", ")), "blueprintPortWire");
                else if (input && !adapter)
                    line = text("Not connected yet", "blueprintPortMissing");
                if (line) {
                    line->setContentsMargins(16, 0, 0, 0);
                    rows->addWidget(line);
                }
            }
            m_ports->addWidget(card);
        }
        m_ports->addSpacing(4);
    }
    m_ports->addStretch();

    clear(m_lastRun);
    if (lastStep.isEmpty()) {
        auto *empty = text(node.isEmpty() ? "Add the node and run the workflow to see its output here."
                                          : "Not run yet. Press Run or Test this node to see its output here.",
                           "blueprintDetailsEmpty");
        empty->setAlignment(Qt::AlignCenter);
        m_lastRun->addWidget(empty);
    } else {
        auto *stats = new QHBoxLayout;
        stats->setSpacing(6);
        const auto stat = [&](const QString &caption, const QString &value, const QString &color = {}) {
            auto *frame = new QFrame;
            frame->setObjectName("blueprintDetailsPort");
            auto *layout = new QVBoxLayout(frame);
            layout->setContentsMargins(10, 8, 10, 8);
            layout->setSpacing(2);
            layout->addWidget(text(caption, "blueprintDetailsCaption"));
            auto *label = text(value, "blueprintDetailsStat");
            if (!color.isEmpty()) label->setStyleSheet(QString("color: %1;").arg(color));
            layout->addWidget(label);
            stats->addWidget(frame, 1);
        };
        const auto status = lastStep["status"].toString();
        stat("Status", BlueprintPreview::statusLabel(status),
             status == "failed" ? Theme::statusError() : status == "passed" || status == "completed" ? Theme::statusSuccess() : QString());
        if (lastStep["duration"].isDouble()) stat("Time", BlueprintPreview::duration(qint64(lastStep["duration"].toDouble())));
        m_lastRun->addLayout(stats);
        if (!lastStep["error"].toString().isEmpty()) m_lastRun->addWidget(text(lastStep["error"].toString(), "blueprintError"));
        if (lastStep.contains("output")) m_lastRun->addWidget(block("Output", BlueprintPreview::jsonText(lastStep["output"])));
    }
    m_lastRun->addStretch();
    m_lastRunTab->setVisible(!node.isEmpty());

    while (auto *item = m_footer->takeAt(0)) {
        if (auto *widget = item->widget()) widget->deleteLater();
        delete item;
    }
    if (node.isEmpty()) {
        auto *add = new Button("Add to canvas");
        add->setObjectName("primaryBtn");
        m_footer->addWidget(add);
        connect(add, &Button::clicked, this, [this] { hide(); emit addRequested(); });
    } else {
        auto *edit = new Button("Edit settings");
        auto *test = new Button("Test this node");
        m_footer->addWidget(edit);
        m_footer->addWidget(test);
        const auto id = m_nodeId;
        connect(edit, &Button::clicked, this, [this, id] { hide(); emit editRequested(id); });
        connect(test, &Button::clicked, this, [this, id] { emit testRequested(id); });
    }
    m_footer->addStretch();

    m_tabs->button(0)->click();
    place(anchor);
    show();
    raise();
}

void BlueprintDetails::place(const QRect &anchor) {
    auto *host = parentWidget();
    if (!host) return;
    QRect area = host->rect();
    if (auto *view = qobject_cast<BlueprintView *>(host->parentWidget())) area = view->freeArea();
    const int width = kWidth + 2 * shadowMargin();
    const int height = qMin(qMax(sizeHint().height(), 520), area.height());
    resize(width, height);
    int x = anchor.right() + 12;
    if (x + width > area.right()) x = anchor.left() - 12 - width;
    x = qBound(area.left(), x, qMax(area.left(), area.right() + 1 - width));
    const int y = qBound(area.top(), anchor.top(), qMax(area.top(), area.bottom() + 1 - height));
    move(x, y);
}

void BlueprintDetails::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton && m_header->geometry().contains(event->position().toPoint()))
        m_dragFrom = event->position().toPoint();
    event->accept();
}

void BlueprintDetails::mouseMoveEvent(QMouseEvent *event) {
    if (m_dragFrom.isNull() || !(event->buttons() & Qt::LeftButton)) return;
    move(pos() + event->position().toPoint() - m_dragFrom);
    event->accept();
}

void BlueprintDetails::mouseReleaseEvent(QMouseEvent *event) {
    m_dragFrom = {};
    event->accept();
}
