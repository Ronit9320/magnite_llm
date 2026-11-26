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
#include <QPainter>
#include <QScrollArea>
#include <QShortcut>
#include <QStackedWidget>
#include <QVBoxLayout>

namespace {
constexpr int kWidth = 372;

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
        delete item;
    }
}

QJsonObject findById(const QJsonArray &array, const QString &id) {
    for (const auto &value : array)
        if (value.toObject()["id"].toString() == id) return value.toObject();
    return {};
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
    body->setSpacing(8);
    m_header = new QWidget;
    m_header->setCursor(Qt::SizeAllCursor);
    auto *header = new QHBoxLayout(m_header);
    header->setContentsMargins(0, 0, 0, 0);
    auto *names = new QVBoxLayout;
    names->setSpacing(2);
    m_title = text({}, "blueprintSection");
    m_subtitle = text({});
    names->addWidget(m_title);
    names->addWidget(m_subtitle);
    header->addLayout(names, 1);
    auto *close = new Button({});
    close->setFlat(true);
    close->setObjectName("blueprintIconButton");
    close->setAccessibleName("Close node details");
    close->setToolTip("Close node details");
    close->setFixedSize(24, 24);
    close->setProperty("themeIcon", "window-close");
    close->setIcon(Theme::coloredIcon("window-close", QColor(Theme::textSecondary()), 14));
    header->addWidget(close, 0, Qt::AlignTop);
    connect(close, &Button::clicked, this, &QWidget::hide);
    body->addWidget(m_header);

    auto *tabs = new QHBoxLayout;
    tabs->setSpacing(4);
    m_tabs = new QButtonGroup(this);
    for (const auto &name : {QString("Overview"), QString("Inputs & outputs"), QString("Last run")}) {
        auto *tab = new Button(QString(name).replace('&', "&&"));
        tab->setObjectName("blueprintSegment");
        tab->setCheckable(true);
        tab->setAccessibleName(name);
        m_tabs->addButton(tab, m_tabs->buttons().size());
        tabs->addWidget(tab);
    }
    body->addLayout(tabs);
    m_pages = new QStackedWidget;
    for (auto **layout : {&m_overview, &m_ports, &m_lastRun}) {
        auto *page = new QWidget;
        *layout = new QVBoxLayout(page);
        (*layout)->setContentsMargins(0, 0, 0, 0);
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
    m_title->setText(node.isEmpty() ? spec["label"].toString(operation) : node["name"].toString());
    m_subtitle->setText(m_preview.nodeKindLabel(kind) + (spec.isEmpty() ? QString() : " · " + spec["label"].toString()));

    clear(m_overview);
    const auto about = doc["about"].toString(spec["detail"].toString());
    m_overview->addWidget(text(about.isEmpty() ? "No description is available for this node yet." : about));
    const auto example = doc["example"].toObject();
    if (!example.isEmpty()) {
        m_overview->addWidget(text("Example", "headingLabel"));
        for (const auto &[label, value] : {std::pair{"inputLabel", "input"}, std::pair{"outputLabel", "output"}}) {
            m_overview->addWidget(text(example[label].toString(), "cardMeta"));
            m_overview->addWidget(text(example[value].toString(), "blueprintCode"));
        }
    }
    const auto tips = doc["tips"].toArray();
    if (!tips.isEmpty()) {
        m_overview->addWidget(text("Tips", "headingLabel"));
        for (const auto &tip : tips) m_overview->addWidget(text("• " + tip.toString()));
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
        for (const auto &value : source[side].toArray()) {
            const auto port = value.toObject();
            const auto type = port["type"].toString();
            auto *row = new QHBoxLayout;
            auto *dot = new QLabel;
            dot->setFixedSize(10, 10);
            QPixmap swatch(10, 10);
            swatch.fill(Qt::transparent);
            {
                QPainter painter(&swatch);
                painter.setRenderHint(QPainter::Antialiasing);
                painter.setPen(Qt::NoPen);
                painter.setBrush(BlueprintStyle::typeColor(type));
                painter.drawEllipse(QRectF(1, 1, 8, 8));
            }
            dot->setPixmap(swatch);
            row->addWidget(dot);
            row->addWidget(text(QString(input ? "In · %1 · %2" : "Out · %1 · %2")
                .arg(m_preview.portLabel(port), contracts.value(type).toObject()["label"].toString(type)), "blueprintSection"), 1);
            m_ports->addLayout(row);
            const auto note = notes[side + "." + port["id"].toString()].toString();
            if (!note.isEmpty()) m_ports->addWidget(text(note));
            if (node.isEmpty()) continue;
            for (const auto &edgeValue : edges) {
                const auto edge = edgeValue.toObject();
                if (input ? (edge["to"] != node["id"] || edge["in"] != port["id"])
                          : (edge["from"] != node["id"] || edge["out"] != port["id"]))
                    continue;
                const auto other = findById(nodes, (input ? edge["from"] : edge["to"]).toString());
                m_ports->addWidget(text(QString(input ? "Wired from %1" : "Wired to %1").arg(other["name"].toString())));
            }
        }
    }
    if (m_ports->isEmpty()) m_ports->addWidget(text("This node has no pins."));
    m_ports->addStretch();

    clear(m_lastRun);
    if (lastStep.isEmpty()) m_lastRun->addWidget(text(node.isEmpty() ? "Add the node and run the workflow to see its output."
                                                                       : "Not run yet"));
    else {
        auto status = BlueprintPreview::statusLabel(lastStep["status"].toString());
        if (lastStep["duration"].isDouble())
            status += " · " + BlueprintPreview::duration(qint64(lastStep["duration"].toDouble()));
        m_lastRun->addWidget(text(status, "blueprintSection"));
        if (!lastStep["error"].toString().isEmpty()) m_lastRun->addWidget(text(lastStep["error"].toString(), "blueprintError"));
        if (lastStep.contains("output")) m_lastRun->addWidget(text(BlueprintPreview::jsonText(lastStep["output"]), "blueprintCode"));
    }
    m_lastRun->addStretch();

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
    const int height = qMin(qMax(sizeHint().height(), 380), area.height());
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
