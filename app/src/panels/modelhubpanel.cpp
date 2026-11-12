#include "panels/modelhubpanel.h"

#include "backend_client.h"
#include "panels/modelcard.h"
#include "theme.h"
#include "widgets/button.h"
#include "widgets/combobox.h"
#include "widgets/flowlayout.h"
#include "widgets/progressbar.h"
#include "widgets/surface.h"
#include "widgets/settingswindow.h"
#include "widgets/tabwidget.h"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QGridLayout>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QPainter>
#include <QKeyEvent>
#include <QScreen>
#include <QResizeEvent>
#include <QPointer>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStyle>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace {
constexpr int pageSize = 30;

QLabel *label(const QString &text, const QString &style, QWidget *parent) {
    auto *widget = new QLabel(text, parent);
    widget->setTextFormat(Qt::PlainText);
    widget->setObjectName(style);
    widget->setWordWrap(true);
    widget->setMinimumWidth(0);
    widget->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    return widget;
}

QString known(const QString &value) {
    return value.isEmpty() ? QStringLiteral("Not reported") : value;
}

QString number(double value) {
    if (value <= 0 || !std::isfinite(value)) return QStringLiteral("Not reported");
    if (value >= 1e9) return QString::number(value / 1e9, 'f', 1) + "B";
    if (value >= 1e6) return QString::number(value / 1e6, 'f', 1) + "M";
    if (value >= 1e3) return QString::number(value / 1e3, 'f', 1) + "K";
    return QString::number(value, 'f', 0);
}

QString tokens(const QJsonValue &value) {
    const qint64 count = static_cast<qint64>(value.toDouble());
    if (count <= 0) return QStringLiteral("Not reported");
    return count % 1024 == 0 ? QString::number(count / 1024) + "K" : QLocale().toString(count);
}

QString bytes(const QJsonValue &value) {
    const auto size = static_cast<qint64>(value.toDouble());
    return value.isDouble() && size >= 0 ? ModelCard::formatBytes(size) : QStringLiteral("Not reported");
}

QString strings(const QJsonValue &value) {
    QStringList result;
    for (const auto &entry : value.toArray()) result << entry.toString();
    return result.isEmpty() ? QStringLiteral("Not reported") : result.join(", ");
}

QString repoId(const QJsonObject &model) {
    return model.value("repo_id").toString();
}

QString localId(QString repo) {
    return "huggingface:" + repo.replace('/', "--");
}

QUrl publisherUrl(const QString &repo) {
    QUrl url("https://huggingface.co");
    url.setPath('/' + repo);
    return url;
}

QFrame *rule(QWidget *parent) {
    auto *line = new QFrame(parent);
    line->setObjectName("hubRule");
    line->setFixedHeight(1);
    return line;
}

QVBoxLayout *surface(QWidget *parent, const QString &title = {}) {
    auto *body = Surface::card(parent);
    body->parentWidget()->setObjectName("hubSurface");
    body->setContentsMargins(22, 21, 22, 21);
    body->setSpacing(14);
    if (!title.isEmpty()) body->addWidget(label(title, "hubSectionTitle", body->parentWidget()));
    return body;
}

QLabel *avatar(const QString &publisher, QWidget *parent) {
    auto *mark = label(publisher.left(1).toUpper(), "hubPublisherAvatar", parent);
    mark->setFixedSize(25, 25);
    mark->setAlignment(Qt::AlignCenter);
    mark->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    return mark;
}

QWidget *publisherRow(const QString &publisher, QWidget *parent) {
    auto *widget = new QWidget(parent);
    auto *layout = new QHBoxLayout(widget);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);
    layout->addWidget(avatar(publisher, widget));
    layout->addWidget(label(publisher, "hubPublisher", widget), 1);
    return widget;
}

// Tags stay on one line; full metadata remains available on hover and in details.
class HubPill : public QLabel {
public:
    HubPill(const QString &text, QWidget *parent) : QLabel(text, parent), m_text(text) {
        setTextFormat(Qt::PlainText);
        setWordWrap(false);
        setIndent(0);
        setToolTip(text);
        setAccessibleName(text);
        setMinimumWidth(0);
        setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Fixed);
    }
    QSize sizeHint() const override {
        ensurePolished();
        return {fontMetrics().horizontalAdvance(m_text) + 16, fontMetrics().height() + 6};
    }
    QSize minimumSizeHint() const override { return {0, sizeHint().height()}; }
protected:
    void resizeEvent(QResizeEvent *event) override {
        QLabel::resizeEvent(event);
        setText(fontMetrics().elidedText(m_text, Qt::ElideRight, qMax(0, width() - 12)));
    }
private:
    QString m_text;
};

QLabel *pill(const QString &text, QWidget *parent, const QString &style = "hubPill") {
    auto *widget = new HubPill(text, parent);
    widget->setObjectName(style);
    return widget;
}

void dialogHeading(QVBoxLayout *layout, const QString &title, QDialog *dialog) {
    auto *header = new QWidget(dialog);
    header->setProperty("windowDragRegion", true);
    auto *row = new QHBoxLayout(header);
    row->setContentsMargins(0, 0, 0, 0);
    row->addWidget(label(title, "hubDialogTitle", header), 1);
    auto *close = new Button(header);
    close->setObjectName("hubDialogClose");
    close->setFlat(true);
    close->setFixedSize(30, 30);
    close->setIconSize(QSize(16, 16));
    close->setIcon(Theme::coloredIcon("window-close", QColor(Theme::textSecondary()), 16));
    close->setAccessibleName("Close " + title);
    close->setToolTip("Close");
    close->setAutoDefault(false);
    QObject::connect(close, &QPushButton::clicked, dialog, &QDialog::reject);
    row->addWidget(close);
    layout->addWidget(header);
}

void addPills(QVBoxLayout *body, const QStringList &values) {
    auto *row = new QHBoxLayout;
    row->setSpacing(6);
    for (const auto &value : values) {
        if (!value.isEmpty() && value != "Not reported") row->addWidget(pill(value, body->parentWidget()));
    }
    row->addStretch();
    body->addLayout(row);
}

QString taskName(const QJsonObject &model) {
    QString name = model.value("pipeline_tag").toString();
    if (name.isEmpty()) return "General model";
    name.replace('-', ' ');
    name[0] = name[0].toUpper();
    return name;
}

QString description(const QJsonObject &model) {
    const QString task = model.value("pipeline_tag").toString();
    if (task == "text-generation") return "Text generation for assistants, writing and language workflows.";
    if (task == "feature-extraction" || task == "sentence-similarity") return "Text representations for semantic search, retrieval and similarity.";
    if (task == "image-text-to-text") return "Visual and text inputs for multimodal language workflows.";
    if (task == "text-to-image") return "Image generation from text prompts with a compatible engine.";
    if (task == "automatic-speech-recognition") return "Speech transcription with the publisher’s supported audio workflow.";
    return "Explore the model’s capabilities, weight formats and local requirements.";
}

QString countLabel(double count) {
    return count == 0 ? QStringLiteral("0") : number(count);
}

Button *modelLink(const QString &repo, QWidget *parent) {
    auto *button = new Button(repo.section('/', 1), parent);
    button->setObjectName("hubModelName");
    button->setProperty("wordWrap", true);
    button->setProperty("leftAligned", true);
    button->setProperty("hubRepo", repo);
    button->setFlat(true);
    button->setMinimumWidth(0);
    auto policy = QSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    button->setSizePolicy(policy);
    button->setAccessibleName("Open model " + repo);
    button->setToolTip(repo);
    button->setAutoDefault(false);
    for (const int key : {int(Qt::Key_Return), int(Qt::Key_Enter)}) {
        auto *activate = new QShortcut(QKeySequence(key), button);
        activate->setContext(Qt::WidgetShortcut);
        QObject::connect(activate, &QShortcut::activated, button, &QPushButton::click);
    }
    return button;
}

void addSpec(QVBoxLayout *layout, const QString &name, const QString &value) {
    auto *frame = new QFrame(layout->parentWidget());
    frame->setObjectName("hubSpecRow");
    auto *row = new QHBoxLayout(frame);
    row->setContentsMargins(0, 10, 0, 10);
    row->setSpacing(18);
    row->addWidget(label(name, "hubMeta", frame), 1);
    auto *text = label(value, "hubValue", frame);
    text->setTextInteractionFlags(Qt::TextSelectableByMouse);
    text->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    row->addWidget(text, 2);
    layout->addWidget(frame);
}

class MemoryBreakdown : public QWidget {
public:
    explicit MemoryBreakdown(QWidget *parent) : QWidget(parent) {
        setFixedHeight(7);
        setAccessibleName("Estimated weight and cache memory");
    }
    void setValues(double weights, double cache, double capacity) {
        m_weights = weights;
        m_cache = cache;
        m_capacity = std::max(capacity, weights + cache);
        update();
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(Theme::meterTrack()));
        painter.drawRoundedRect(rect(), 2, 2);
        if (m_capacity <= 0) return;
        const double weights = width() * m_weights / m_capacity;
        const double cache = width() * m_cache / m_capacity;
        painter.fillRect(QRectF(0, 0, weights, height()), QColor(Theme::accentPrimary()));
        painter.fillRect(QRectF(weights, 0, cache, height()), QColor(Theme::comparisonSeries()));
    }
private:
    double m_weights = 0;
    double m_cache = 0;
    double m_capacity = 0;
};

void clearLayout(QLayout *layout) {
    while (auto *item = layout->takeAt(0)) {
        if (auto *child = item->layout()) clearLayout(child);
        if (auto *widget = item->widget()) {
            widget->hide();
            widget->deleteLater();
        }
        delete item;
    }
}

QWidget *scrollPage(QWidget *body, QWidget *parent) {
    auto *scroll = new QScrollArea(parent);
    scroll->setObjectName("cardScroll");
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->viewport()->setAutoFillBackground(false);
    scroll->setWidget(body);
    return scroll;
}

QTableWidget *table(const QStringList &headers, QWidget *parent) {
    auto *widget = new QTableWidget(0, headers.size(), parent);
    widget->setHorizontalHeaderLabels(headers);
    widget->horizontalHeader()->setSectionResizeMode(QHeaderView::Stretch);
    widget->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    widget->verticalHeader()->hide();
    widget->setEditTriggers(QAbstractItemView::NoEditTriggers);
    widget->setSelectionBehavior(QAbstractItemView::SelectRows);
    widget->setWordWrap(true);
    widget->setShowGrid(false);
    widget->setMinimumHeight(220);
    return widget;
}
}

ModelHubPanel::ModelHubPanel(QWidget *parent)
    : BasePanel("Model hub", parent, true) {
    setObjectName("modelHubPanel");
    m_pages = new QStackedWidget(this);
    contentLayout()->addWidget(m_pages, 1);
    setupDiscovery();
    m_details = new QWidget(m_pages);
    m_detailLayout = new QVBoxLayout(m_details);
    m_detailLayout->setContentsMargins(0, 0, 0, 0);
    m_detailLayout->setSpacing(18);
    m_pages->addWidget(scrollPage(m_details, m_pages));
    m_downloadStrip = new QFrame(this);
    m_downloadStrip->setObjectName("hubDownloadStrip");
    auto *download = new QHBoxLayout(m_downloadStrip);
    download->setContentsMargins(14, 10, 14, 10);
    m_downloadText = label({}, "hubMeta", m_downloadStrip);
    m_progress = new ProgressBar(m_downloadStrip);
    m_progress->setRange(0, 100);
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(5);
    m_cancelButton = new Button("Cancel download", m_downloadStrip);
    m_cancelButton->setObjectName("hubCancelDownload");
    download->addWidget(m_downloadText, 2);
    download->addWidget(m_progress, 1);
    download->addWidget(m_cancelButton);
    contentLayout()->addWidget(m_downloadStrip);
    m_downloadStrip->hide();
    m_status = label("Connect to the backend to discover models.", "statusLabel", this);
    contentLayout()->addWidget(m_status);
    connect(m_cancelButton, &QPushButton::clicked, this, &ModelHubPanel::cancelDownload);
    m_pullTimer = new QTimer(this);
    m_pullTimer->setInterval(400);
    connect(m_pullTimer, &QTimer::timeout, this, &ModelHubPanel::pollDownload);
    auto *find = new QShortcut(QKeySequence::Find, this);
    find->setContext(Qt::WidgetWithChildrenShortcut);
    connect(find, &QShortcut::activated, this, [this] { backToDiscovery(); m_search->setFocus(); m_search->selectAll(); });
    auto *back = new QShortcut(QKeySequence(Qt::ALT | Qt::Key_Left), this);
    back->setContext(Qt::WidgetWithChildrenShortcut);
    connect(back, &QShortcut::activated, this, &ModelHubPanel::backToDiscovery);
}

void ModelHubPanel::setupDiscovery() {
    m_discovery = new QWidget(m_pages);
    auto *layout = new QVBoxLayout(m_discovery);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(18);
    layout->addWidget(label("Model hub", "hubPageTitle", m_discovery));
    layout->addWidget(label("Find, understand and run the right model for your work.", "panelSubtitle", m_discovery));
    auto *deviceCard = new QFrame(m_discovery);
    deviceCard->setObjectName("hubDeviceCard");
    auto *device = new QHBoxLayout(deviceCard);
    device->setContentsMargins(18, 15, 18, 15);
    device->setSpacing(14);
    auto *deviceIcon = label({}, "hubDeviceIcon", deviceCard);
    deviceIcon->setProperty("themeIcon", "cpu");
    deviceIcon->setPixmap(Theme::coloredIcon("cpu", QColor(Theme::textSecondary()), 22).pixmap(22, 22));
    deviceIcon->setFixedSize(28, 28);
    deviceIcon->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    device->addWidget(deviceIcon);
    auto *deviceName = new QVBoxLayout;
    deviceName->setSpacing(4);
    deviceName->addWidget(label("Your device", "hubDeviceTitle", deviceCard));
    m_device = label("Waiting for hardware readings…", "hubSmallMeta", deviceCard);
    deviceName->addWidget(m_device);
    device->addLayout(deviceName, 2);
    auto *memory = new QVBoxLayout;
    memory->setSpacing(7);
    m_deviceMeta = label("Memory unavailable", "hubSmallMeta", deviceCard);
    memory->addWidget(m_deviceMeta);
    m_deviceUsage = new ProgressBar(deviceCard);
    m_deviceUsage->setObjectName("hubDeviceUsage");
    m_deviceUsage->setAccessibleName("Device memory in use");
    m_deviceUsage->setRange(0, 100);
    m_deviceUsage->setTextVisible(false);
    m_deviceUsage->setFixedHeight(5);
    m_deviceUsage->hide();
    memory->addWidget(m_deviceUsage);
    device->addLayout(memory, 1);
    m_deviceButton = new Button("Device details", deviceCard);
    m_deviceButton->setObjectName("hubDeviceDetails");
    m_deviceButton->setFlat(true);
    m_deviceButton->setMinimumWidth(112);
    connect(m_deviceButton, &QPushButton::clicked, this, &ModelHubPanel::showDeviceDetails);
    device->addWidget(m_deviceButton);
    layout->addWidget(deviceCard);

    auto *search = new QHBoxLayout;
    m_search = new QLineEdit(m_discovery);
    m_search->setObjectName("hubSearch");
    m_search->setAccessibleName("Search Hub models");
    m_search->setPlaceholderText("Search models, publishers or capabilities");
    m_search->setClearButtonEnabled(true);
    m_search->addAction(Theme::coloredIcon("search", QColor(Theme::textMuted()), 16), QLineEdit::LeadingPosition);
    m_searchButton = new Button("Search", m_discovery);
    m_searchButton->setObjectName("primaryBtn");
    m_filterButton = new Button("Filters", m_discovery);
    m_filterButton->setObjectName("hubFiltersToggle");
    m_filterButton->setAccessibleName("Show model filters");
    m_filterButton->setCheckable(true);
    m_filterButton->setProperty("expanded", false);
    m_sort = new ComboBox(m_discovery);
    m_sort->setObjectName("hubSort");
    m_sort->setAccessibleName("Sort loaded models");
    m_sort->addItems({"Most downloaded", "Most liked", "Smallest repository", "Name A–Z"});
    search->addWidget(m_search, 1);
    search->addWidget(m_filterButton);
    search->addWidget(m_searchButton);
    search->addWidget(m_sort);
    layout->addLayout(search);
    m_filters = new QFrame(m_discovery);
    m_filters->setObjectName("hubFilters");
    auto *filters = new QHBoxLayout(m_filters);
    filters->setContentsMargins(18, 16, 18, 16);
    filters->setSpacing(18);
    auto addFilter = [this, filters](const QString &title, ComboBox *&combo) {
        auto *column = new QVBoxLayout;
        column->setSpacing(8);
        combo = new ComboBox(m_filters);
        combo->setMinimumWidth(0);
        combo->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
        combo->setAccessibleName(title);
        auto *caption = label(title, "hubMeta", m_filters);
        caption->setBuddy(combo);
        column->addWidget(caption);
        column->addWidget(combo);
        filters->addLayout(column, title == "Publisher" ? 5 : 4);
        connect(combo, &QComboBox::currentIndexChanged, this, &ModelHubPanel::renderCards);
    };
    addFilter("Task", m_task);
    addFilter("Publisher", m_publisher);
    addFilter("Weight format", m_format);
    m_task->addItem("All tasks", "");
    m_publisher->addItem("All publishers", "");
    m_format->addItem("All formats", "");
    m_format->addItem("GGUF", "gguf");
    m_format->addItem("Safetensors", "safetensors");
    auto *availability = new QVBoxLayout;
    availability->addWidget(label("Availability", "hubMeta", m_filters));
    m_localOnly = new QCheckBox("Available locally", m_filters);
    m_localOnly->setObjectName("hubLocalOnly");
    m_localOnly->setMinimumHeight(36);
    availability->setSpacing(8);
    availability->addWidget(m_localOnly);
    auto *reset = new Button("Reset filters", m_filters);
    reset->setObjectName("hubResetFilters");
    filters->addLayout(availability);
    filters->addWidget(reset, 0, Qt::AlignBottom);
    layout->addWidget(m_filters);
    m_filters->hide();
    m_resultCount = label("Discover models from Hugging Face", "hubMeta", m_discovery);
    layout->addWidget(m_resultCount);
    m_cardHost = new QWidget(m_discovery);
    m_flow = new FlowLayout(m_cardHost, 14, 14);
    m_flow->setColumnMinimumWidth(380);
    m_flow->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_cardHost);
    m_empty = label("Search a model family, or refresh to browse the most downloaded models.", "rackEmpty", m_discovery);
    m_empty->setAlignment(Qt::AlignCenter);
    m_empty->setMinimumHeight(160);
    layout->addWidget(m_empty);
    m_loadMore = new Button("Load more models", m_discovery);
    m_loadMore->setObjectName("hubLoadMore");
    layout->addWidget(m_loadMore, 0, Qt::AlignHCenter);
    m_loadMore->hide();
    m_compareTray = new QFrame(m_discovery);
    m_compareTray->setObjectName("hubCompareTray");
    auto *compare = new QHBoxLayout(m_compareTray);
    m_compareCount = label({}, "hubMeta", m_compareTray);
    m_compareButton = new Button("Compare models", m_compareTray);
    m_compareButton->setObjectName("hubCompareModels");
    auto *clear = new Button("Clear comparison", m_compareTray);
    clear->setObjectName("hubClearComparison");
    compare->addWidget(m_compareCount, 1);
    compare->addWidget(m_compareButton);
    compare->addWidget(clear);
    contentLayout()->addWidget(m_compareTray);
    m_compareTray->hide();
    layout->addStretch();
    m_pages->addWidget(scrollPage(m_discovery, m_pages));

    connect(m_searchButton, &QPushButton::clicked, this, [this] { this->search(); });
    connect(m_search, &QLineEdit::returnPressed, this, [this] { this->search(); });
    connect(m_loadMore, &QPushButton::clicked, this, [this] { this->search(true); });
    connect(m_sort, &QComboBox::currentIndexChanged, this, &ModelHubPanel::renderCards);
    connect(m_localOnly, &QCheckBox::toggled, this, &ModelHubPanel::renderCards);
    connect(reset, &QPushButton::clicked, this, &ModelHubPanel::resetFilters);
    connect(m_filterButton, &QPushButton::toggled, this, [this](bool expanded) {
        m_filters->setVisible(expanded);
        m_filterButton->setProperty("expanded", expanded);
        m_filterButton->setAccessibleName(expanded ? "Hide model filters" : "Show model filters");
    });
    connect(m_compareButton, &QPushButton::clicked, this, &ModelHubPanel::showComparison);
    connect(clear, &QPushButton::clicked, this, [this] { m_compared.clear(); m_comparisonData.clear(); renderCards();
        if (m_pages->currentIndex() == 1) renderDetails(); });
}

void ModelHubPanel::request(const QString &path, Reply reply) {
    if (!m_client || !m_client->isReady()) return;
    QPointer<ModelHubPanel> guard(this);
    m_client->get(path, [guard, reply = std::move(reply)](int code, const QJsonDocument &document) {
        if (guard) reply(code, document);
    });
}

void ModelHubPanel::setBackendClient(BackendClient *client) {
    m_client = client;
    refreshDevice();
    search();
}

void ModelHubPanel::refresh() {
    refreshDevice();
    if (!m_searching) search();
}

void ModelHubPanel::showEvent(QShowEvent *event) {
    BasePanel::showEvent(event);
    refreshDevice();
    refreshLocalState();
    if (!m_loaded && !m_searching && m_client && m_client->isReady()) search();
}

void ModelHubPanel::refreshLocalState() {
    if (m_models.isEmpty() && m_detailRepo.isEmpty()) return;
    request("/models?source=huggingface", [this](int code, const QJsonDocument &document) {
        if (code != 200 || !document.isArray()) return;
        QSet<QString> local;
        for (const auto &entry : document.array()) {
            const auto model = entry.toObject();
            if (model.value("status").toString("ready") == "ready") local.insert(model.value("ref").toString().toLower());
        }
        for (auto &entry : m_models) entry["downloaded"] = local.contains(repoId(entry).toLower()) && repoId(entry) != m_pullRepo;
        for (auto it = m_metadata.begin(); it != m_metadata.end(); ++it) it.value()["downloaded"] = local.contains(it.key().toLower()) && it.key() != m_pullRepo;
        if (!m_detailRepo.isEmpty()) m_detailData["downloaded"] = local.contains(m_detailRepo.toLower()) && m_detailRepo != m_pullRepo;
        updateDownloadUi();
        if (m_pages->currentIndex() == 0) renderCards();
    });
}

void ModelHubPanel::search(bool append) {
    if (!m_client || !m_client->isReady()) { status("Backend not connected."); return; }
    if (append && (m_searching || !m_hasMore)) return;
    const auto generation = ++m_searchGeneration;
    m_searching = true;
    m_loaded = true;
    if (!append) m_query = m_search->text().trimmed();
    const int offset = append ? m_offset : 0;
    QUrlQuery query;
    query.addQueryItem("q", m_query);
    query.addQueryItem("limit", QString::number(pageSize));
    query.addQueryItem("offset", QString::number(offset));
    m_searchButton->setText("Searching…");
    m_loadMore->setEnabled(false);
    status("Searching Hugging Face…");
    request("/models/discover?" + query.toString(QUrl::FullyEncoded), [this, generation, append, offset](int code, const QJsonDocument &document) {
        if (generation != m_searchGeneration) return;
        m_searching = false;
        m_searchButton->setText("Search");
        m_loadMore->setEnabled(true);
        if (code != 200 || !document.isArray()) {
            status("Could not search the Hub. " + document.object().value("detail").toString(), "error");
            return;
        }
        if (!append) m_models.clear();
        for (const auto &value : document.array()) {
            const auto model = value.toObject();
            const auto repo = repoId(model);
            if (repo.isEmpty()) continue;
            auto existing = std::find_if(m_models.begin(), m_models.end(), [&repo](const QJsonObject &entry) { return repoId(entry) == repo; });
            if (existing == m_models.end()) m_models.append(model);
        }
        m_offset = offset + document.array().size();
        m_hasMore = document.array().size() == pageSize && m_offset <= 1000;
        updateFilterChoices();
        renderCards();
        status(QString("%1 models loaded from Hugging Face. Filters apply to loaded results.").arg(m_models.size()));
        QUrlQuery sizes;
        for (const auto &value : document.array()) sizes.addQueryItem("repo", repoId(value.toObject()));
        if (document.array().isEmpty()) return;
        request("/models/sizes?" + sizes.toString(QUrl::FullyEncoded), [this](int sizeCode, const QJsonDocument &sizeDoc) {
            if (sizeCode != 200) return;
            const auto data = sizeDoc.object();
            for (auto &model : m_models) {
                if (data.contains(repoId(model))) model["total_bytes"] = data.value(repoId(model));
            }
            renderCards();
        });
    });
}

void ModelHubPanel::updateFilterChoices() {
    auto fill = [this](ComboBox *combo, const QString &all, bool publisher) {
        const auto selected = combo->currentData().toString();
        QSignalBlocker block(combo);
        QSet<QString> values;
        for (const auto &model : m_models) {
            const auto value = publisher ? repoId(model).section('/', 0, 0) : model.value("pipeline_tag").toString();
            if (!value.isEmpty()) values.insert(value);
        }
        QStringList sorted = values.values();
        sorted.sort(Qt::CaseInsensitive);
        combo->clear();
        combo->addItem(all, "");
        for (const auto &value : sorted) combo->addItem(value, value);
        const int index = combo->findData(selected);
        combo->setCurrentIndex(index < 0 ? 0 : index);
    };
    fill(m_task, "All tasks", false);
    fill(m_publisher, "All publishers", true);
}

void ModelHubPanel::resetFilters() {
    const QSignalBlocker task(m_task), publisher(m_publisher), format(m_format), local(m_localOnly);
    m_task->setCurrentIndex(0);
    m_publisher->setCurrentIndex(0);
    m_format->setCurrentIndex(0);
    m_localOnly->setChecked(false);
    renderCards();
}

void ModelHubPanel::renderCards() {
    if (!m_flow || !m_task || !m_publisher || !m_format || !m_localOnly) return;
    QString focusedRepo, focusedAction;
    if (auto *focus = QApplication::focusWidget()) {
        focusedRepo = focus->property("hubRepo").toString();
        focusedAction = focus->objectName();
    }
    clearLayout(m_flow);
    auto sorted = m_models;
    const int sort = m_sort->currentIndex();
    std::stable_sort(sorted.begin(), sorted.end(), [sort](const QJsonObject &left, const QJsonObject &right) {
        if (sort == 3) return repoId(left).compare(repoId(right), Qt::CaseInsensitive) < 0;
        if (sort == 2) {
            const double a = left.value("total_bytes").toDouble(-1), b = right.value("total_bytes").toDouble(-1);
            return a >= 0 && (b < 0 || a < b);
        }
        return left.value(sort == 1 ? "likes" : "downloads").toDouble() > right.value(sort == 1 ? "likes" : "downloads").toDouble();
    });
    int shown = 0;
    for (const auto &model : sorted) {
        const auto repo = repoId(model);
        if (!m_task->currentData().toString().isEmpty() && model.value("pipeline_tag").toString() != m_task->currentData().toString()) continue;
        if (!m_publisher->currentData().toString().isEmpty() && repo.section('/', 0, 0) != m_publisher->currentData().toString()) continue;
        if (!m_format->currentData().toString().isEmpty() && !model.value("formats").toArray().contains(m_format->currentData().toString())) continue;
        if (m_localOnly->isChecked() && !model.value("downloaded").toBool()) continue;
        ++shown;
        auto *frame = new QFrame(m_cardHost);
        frame->setObjectName("hubModelCard");
        frame->setProperty("repo", repo);
        frame->setProperty("selected", m_compared.contains(repo));
        frame->setMinimumWidth(300);
        auto *body = new QVBoxLayout(frame);
        body->setContentsMargins(18, 16, 18, 14);
        body->setSpacing(10);
        auto *top = new QHBoxLayout;
        top->addWidget(publisherRow(repo.section('/', 0, 0), frame), 1);
        if (model.value("downloaded").toBool()) top->addWidget(pill("✓ Local", frame, "hubLocalPill"));
        auto *compare = new Button("Compare", frame);
        compare->setCheckable(true);
        compare->setFlat(false);
        compare->setFixedHeight(28);
        compare->setMinimumWidth(88);
        compare->setAutoDefault(false);
        compare->setObjectName("hubCardCompare");
        compare->setProperty("hubRepo", repo);
        compare->setAccessibleName("Compare " + repo);
        compare->setToolTip("Select up to three models to compare");
        compare->setChecked(m_compared.contains(repo));
        compare->setText(m_compared.contains(repo) ? "Added ✓" : "Compare");
        connect(compare, &QPushButton::toggled, this, [this, repo, frame, compare](bool checked) {
            toggleComparison(repo, checked);
            compare->setText(m_compared.contains(repo) ? "Added ✓" : "Compare");
            frame->setProperty("selected", m_compared.contains(repo));
            frame->style()->unpolish(frame);
            frame->style()->polish(frame);
            frame->update();
        });
        top->addWidget(compare);
        body->addLayout(top);
        auto *name = modelLink(repo, frame);
        connect(name, &QPushButton::clicked, this, [this, repo] { openDetails(repo); });
        body->addWidget(name);
        auto *summary = label(description(model), "hubCardDescription", frame);
        summary->setMinimumHeight(34);
        body->addWidget(summary);
        QStringList tags{taskName(model)};
        if (model.value("parameters").toDouble() > 0) tags << number(model.value("parameters").toDouble());
        if (!model.value("license").toString().isEmpty()) tags << model.value("license").toString();
        addPills(body, tags);
        auto *metrics = new QHBoxLayout;
        const double parameters = model.value("parameters").toDouble();
        for (const auto &pair : QList<QPair<QString, QString>>{
                 {"Max. context", tokens(model.value("context_length"))},
                 {"Repository size", bytes(model.value("total_bytes"))},
                 {"FP16 weights", parameters > 0 ? ModelCard::formatBytes(static_cast<qint64>(parameters * 2)) : "Not reported"}}) {
            auto *column = new QVBoxLayout;
            column->setSpacing(6);
            column->addWidget(label(pair.first, "hubSmallMeta", frame));
            column->addWidget(label(pair.second, "hubCardValue", frame));
            metrics->addLayout(column, 1);
        }
        body->addLayout(metrics);
        auto *memoryNote = label("View the model for weight variants and a device memory estimate.", "hubSmallMeta", frame);
        memoryNote->setToolTip("FP16 weight size assumes two bytes per parameter. Cache and runtime overhead need additional memory.");
        body->addWidget(memoryNote);
        body->addStretch();
        body->addWidget(rule(frame));
        auto *popularity = new QHBoxLayout;
        popularity->setSpacing(6);
        auto *downloadIcon = label({}, "hubSmallMeta", frame);
        downloadIcon->setProperty("themeIcon", "download");
        downloadIcon->setPixmap(Theme::coloredIcon("download", QColor(Theme::textMuted()), 13).pixmap(13, 13));
        downloadIcon->setFixedSize(14, 14);
        downloadIcon->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        popularity->addWidget(downloadIcon);
        popularity->addWidget(label(countLabel(model.value("downloads").toDouble()) + "  ·  " + countLabel(model.value("likes").toDouble()) + " likes", "hubSmallMeta", frame), 1);
        auto *details = new Button("View model →", frame);
        details->setObjectName("hubViewDetails");
        details->setFlat(true);
        details->setMinimumWidth(100);
        details->setProperty("hubRepo", repo);
        details->setAccessibleName("View details for " + repo);
        connect(details, &QPushButton::clicked, this, [this, repo] { openDetails(repo); });
        popularity->addWidget(details);
        body->addLayout(popularity);
        m_flow->addWidget(frame);
        frame->show();
        if (focusedRepo == repo) {
            QWidget *target = focusedAction == "hubCardCompare" ? static_cast<QWidget *>(compare)
                : focusedAction == "hubModelName" ? static_cast<QWidget *>(name) : details;
            target->setFocus(Qt::OtherFocusReason);
        }
    }
    const int active = int(m_task->currentIndex() > 0) + int(m_publisher->currentIndex() > 0) + int(m_format->currentIndex() > 0) + int(m_localOnly->isChecked());
    m_filterButton->setText(active ? QString("Filters · %1").arg(active) : "Filters");
    m_resultCount->setText(QString("%1 of %2 loaded models").arg(shown).arg(m_models.size()));
    m_empty->setVisible(shown == 0);
    m_empty->setText(m_models.isEmpty() ? "No models loaded. Search or refresh to try again." : "No loaded models match these filters. Reset filters or load more results.");
    m_loadMore->setVisible(m_hasMore);
    m_compareTray->setVisible(!m_compared.isEmpty());
    m_compareCount->setText(m_compared.size() == 1 ? QString("1 of 3 selected · Select one more to compare") : QString("%1 of 3 models selected").arg(m_compared.size()));
    m_compareButton->setEnabled(m_compared.size() >= 2);
    m_cardHost->updateGeometry();
}

void ModelHubPanel::toggleComparison(const QString &repo, bool checked) {
    if (!checked) {
        m_compared.remove(repo);
        m_comparisonData.remove(repo);
    } else if (m_compared.size() < 3) {
        m_compared.insert(repo);
        for (const auto &model : m_models) if (repoId(model) == repo) m_comparisonData.insert(repo, model);
    }
    else { status("Compare up to three models. Remove one to add another."); renderCards(); return; }
    m_compareTray->setVisible(!m_compared.isEmpty());
    m_compareCount->setText(m_compared.size() == 1 ? QString("1 of 3 selected · Select one more to compare") : QString("%1 of 3 models selected").arg(m_compared.size()));
    m_compareButton->setEnabled(m_compared.size() >= 2);
}

void ModelHubPanel::showComparison() {
    if (m_compared.size() < 2) return;
    SettingsDetailDialog dialog(this);
    dialog.setObjectName("hubCompareDialog");
    dialog.setWindowTitle("Compare models");
    dialog.setMinimumSize(720, 480);
    dialog.resize(std::clamp(window()->width() - 80, 760, 1100), std::clamp(window()->height() - 80, 500, 800));
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 22, 24, 20);
    layout->setSpacing(16);
    dialogHeading(layout, "Compare models", &dialog);
    layout->addWidget(label("Reported specifications for your shortlist. Memory figures use an FP16 weight assumption.", "hubMeta", &dialog));
    QStringList repos = m_compared.values();
    repos.sort();
    auto *body = new QWidget(&dialog);
    auto *grid = new QGridLayout(body);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setSpacing(0);
    grid->setColumnMinimumWidth(0, 120);
    grid->addWidget(label("Model", "hubSmallMeta", body), 0, 0);
    const QStringList fields = {"Parameters", "Max. context", "Repository size", "FP16 weights", "License", "Task", "Weight formats", "Languages"};
    for (int row = 0; row < fields.size(); ++row) {
        auto *cell = new QFrame(body);
        cell->setObjectName("hubComparisonCell");
        auto *cellLayout = new QVBoxLayout(cell);
        cellLayout->setContentsMargins(0, 15, 12, 15);
        cellLayout->addWidget(label(fields[row], "hubMeta", cell));
        grid->addWidget(cell, row + 1, 0);
    }
    for (int column = 0; column < repos.size(); ++column) {
        const auto repo = repos[column];
        QJsonObject model = m_metadata.value(repo);
        if (model.isEmpty()) {
            for (const auto &entry : m_models) if (repoId(entry) == repo) model = entry;
        }
        if (model.isEmpty()) model = m_comparisonData.value(repo);
        auto *header = new QFrame(body);
        header->setObjectName("hubComparisonHeader");
        auto *heading = new QVBoxLayout(header);
        heading->setContentsMargins(14, 10, 14, 18);
        heading->setSpacing(8);
        heading->addWidget(publisherRow(repo.section('/', 0, 0), header));
        auto *name = modelLink(repo, header);
        name->setObjectName("hubComparisonModelName");
        heading->addWidget(name);
        heading->addStretch();
        connect(name, &QPushButton::clicked, &dialog, [this, &dialog, repo] {
            dialog.accept();
            openDetails(repo);
        });
        grid->addWidget(header, 0, column + 1);
        grid->setColumnStretch(column + 1, 1);
        const double parameters = model.value("parameters").toDouble();
        const QStringList values = {number(parameters), tokens(model.value("context_length")), bytes(model.value("total_bytes")),
            parameters > 0 ? ModelCard::formatBytes(static_cast<qint64>(parameters * 2)) : "Not reported",
            known(model.value("license").toString()), taskName(model), strings(model.value("formats")), strings(model.value("languages"))};
        for (int row = 0; row < values.size(); ++row) {
            auto *cell = new QFrame(body);
            cell->setObjectName("hubComparisonCell");
            auto *cellLayout = new QVBoxLayout(cell);
            cellLayout->setContentsMargins(14, 15, 14, 15);
            auto *value = label(values[row], "hubValue", cell);
            value->setTextInteractionFlags(Qt::TextSelectableByMouse);
            cellLayout->addWidget(value);
            grid->addWidget(cell, row + 1, column + 1);
        }
    }
    auto *scroll = scrollPage(body, &dialog);
    scroll->setObjectName("hubComparisonScroll");
    layout->addWidget(scroll, 1);
    layout->addWidget(label("FP16 weights exclude KV cache and runtime overhead. Check each model’s local setup for a context-specific estimate.", "hubSmallMeta", &dialog));
    auto *buttons = new QHBoxLayout;
    buttons->addStretch();
    auto *close = new Button("Close", &dialog);
    close->setObjectName("hubCloseComparison");
    connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
    buttons->addWidget(close);
    layout->addLayout(buttons);
    dialog.exec();
}

void ModelHubPanel::openDetails(const QString &repo) {
    status({});
    m_detailRepo = repo;
    m_detailData = m_metadata.value(repo);
    if (m_detailData.isEmpty()) for (const auto &entry : m_models) if (repoId(entry) == repo) m_detailData = entry;
    m_detailError.clear();
    m_detailLoading = true;
    const auto generation = ++m_detailGeneration;
    m_pages->setCurrentIndex(1);
    renderDetails();
    if (auto *scroll = qobject_cast<QScrollArea *>(m_pages->widget(1))) scroll->verticalScrollBar()->setValue(0);
    QUrlQuery query;
    query.addQueryItem("repo", repo);
    request("/models/hub-detail?" + query.toString(QUrl::FullyEncoded), [this, repo, generation](int code, const QJsonDocument &document) {
        if (generation != m_detailGeneration || repo != m_detailRepo) return;
        m_detailLoading = false;
        if (code == 200 && document.isObject()) {
            m_detailData = document.object();
            m_metadata.insert(repo, m_detailData);
            for (auto &model : m_models) if (repoId(model) == repo) model = m_detailData;
        } else {
            m_detailError = document.object().value("detail").toString();
            if (m_detailError.isEmpty()) m_detailError = QString("Model metadata is unavailable (HTTP %1). Retry to reconnect.").arg(code);
        }
        renderDetails();
    });
}

void ModelHubPanel::renderDetails() {
    const bool sameModel = m_renderedRepo == m_detailRepo;
    const int previousTab = sameModel && m_tabs ? m_tabs->currentIndex() : 0;
    const bool hadWeightChoices = sameModel && m_weight && m_weight->count() > 1;
    const QString previousWeight = sameModel && m_weight ? m_weight->currentText() : QString();
    const int previousContext = sameModel && m_context ? m_context->currentData().toInt() : 8192;
    m_renderedRepo = m_detailRepo;
    clearLayout(m_detailLayout);
    m_detailAction = nullptr;
    m_detailStatus = nullptr;
    m_weight = nullptr;
    m_context = nullptr;
    m_estimate = nullptr;
    m_memoryBar = nullptr;
    m_memoryStatus = nullptr;
    m_estimateDetail = nullptr;
    m_detailDevice = nullptr;
    auto *back = new Button("← Back to models", m_details);
    back->setFlat(true);
    back->setObjectName("hubBack");
    connect(back, &QPushButton::clicked, this, &ModelHubPanel::backToDiscovery);
    m_detailLayout->addWidget(back, 0, Qt::AlignLeft);
    auto *heading = new QHBoxLayout;
    heading->setSpacing(22);
    auto *identity = new QVBoxLayout;
    identity->setSpacing(10);
    identity->addWidget(publisherRow(m_detailRepo.section('/', 0, 0), m_details));
    identity->addWidget(label(m_detailRepo.section('/', 1), "hubDetailTitle", m_details));
    identity->addWidget(label(description(m_detailData), "hubMeta", m_details));
    QStringList tags{taskName(m_detailData)};
    const auto languages = m_detailData.value("languages").toArray();
    if (!languages.isEmpty()) tags << (languages.size() > 3 ? QString("%1 languages").arg(languages.size()) : strings(languages));
    if (!m_detailData.value("architecture").toString().isEmpty()) tags << m_detailData.value("architecture").toString();
    auto *identityBody = new QWidget(m_details);
    identityBody->setLayout(identity);
    identity->setContentsMargins(0, 0, 0, 0);
    addPills(identity, tags);
    heading->addWidget(identityBody, 1);
    auto *compare = new Button(m_compared.contains(m_detailRepo) ? "Added to comparison" : "Compare", m_details);
    compare->setObjectName("hubDetailCompare");
    compare->setCheckable(true);
    compare->setChecked(m_compared.contains(m_detailRepo));
    connect(compare, &QPushButton::clicked, this, [this, compare](bool checked) {
        toggleComparison(m_detailRepo, checked);
        const QSignalBlocker block(compare);
        compare->setChecked(m_compared.contains(m_detailRepo));
        compare->setText(compare->isChecked() ? "Added to comparison" : "Compare");
    });
    auto *actions = new QVBoxLayout;
    actions->setSpacing(8);
    m_detailAction = new Button({}, m_details);
    m_detailAction->setObjectName("hubDownloadModel");
    m_detailAction->setProperty("primary", true);
    actions->addWidget(m_detailAction);
    actions->addWidget(compare);
    actions->addStretch();
    heading->addLayout(actions);
    m_detailLayout->addLayout(heading);

    auto *stats = new QFrame(m_details);
    stats->setObjectName("hubStats");
    auto *statRow = new QHBoxLayout(stats);
    statRow->setContentsMargins(0, 0, 0, 0);
    statRow->setSpacing(0);
    for (const auto &entry : QList<QPair<QString, QString>>{
             {"Parameters", number(m_detailData.value("parameters").toDouble())},
             {"Max. context", tokens(m_detailData.value("context_length")) + (m_detailData.value("context_length").toDouble() > 0 ? " tokens" : "")},
             {"License", known(m_detailData.value("license").toString())},
             {"Downloads", countLabel(m_detailData.value("downloads").toDouble())}}) {
        auto *cell = new QFrame(stats);
        cell->setObjectName("hubStatCell");
        cell->setProperty("first", statRow->count() == 0);
        auto *column = new QVBoxLayout(cell);
        column->setContentsMargins(20, 17, 20, 17);
        column->setSpacing(7);
        column->addWidget(label(entry.first, "hubSmallMeta", cell));
        column->addWidget(label(entry.second, "hubStatValue", cell));
        column->addWidget(label(entry.first == "Downloads" ? "Publisher’s monthly count" : entry.first == "Max. context" ? "Published model capacity" : entry.first == "License" ? "Review publisher terms" : known(m_detailData.value("architecture").toString()) + " architecture", "hubSmallMeta", cell));
        statRow->addWidget(cell, 1);
    }
    m_detailLayout->addWidget(stats);
    auto *columns = new QHBoxLayout;
    columns->setSpacing(24);
    m_tabs = new TabWidget(m_details);
    m_tabs->setObjectName("hubDetailTabs");
    m_tabs->setDocumentMode(true);
    m_tabs->tabBar()->setProperty("underline", true);
    m_tabs->tabBar()->setElideMode(Qt::ElideNone);
    auto *overview = new QWidget(m_tabs);
    auto *overviewLayout = new QVBoxLayout(overview);
    overviewLayout->setContentsMargins(0, 18, 0, 0);
    overviewLayout->setSpacing(18);
    auto *story = surface(overview, "What you can do with it");
    story->addWidget(label(description(m_detailData) + " Review the publisher’s model card, then evaluate it with prompts and data from your own workflow.", "hubBody", story->parentWidget()));
    auto *uses = new QHBoxLayout;
    uses->setSpacing(12);
    const bool languageModel = m_detailData.value("pipeline_tag").toString() == "text-generation";
    const QList<QStringList> useCases = {
        {languageModel ? "message-circle" : "layers", languageModel ? "Conversation and writing" : "Explore the model’s task", languageModel ? "Draft, summarize and follow multi-step instructions with suitable weights." : "Review the publisher’s intended inputs, outputs and supported engine."},
        {"file-text", languageModel ? "Work with your documents" : "Inspect the model artifacts", languageModel ? "Pair local inference with retrieval to bring your material into the conversation." : "Compare available weight formats and review the model configuration."}
    };
    for (const auto &use : useCases) {
        auto *card = new QFrame(story->parentWidget());
        card->setObjectName("hubUseCase");
        auto *body = new QVBoxLayout(card);
        body->setContentsMargins(15, 15, 15, 15);
        body->setSpacing(9);
        auto *mark = label({}, "hubAccentIcon", card);
        mark->setFixedSize(20, 20);
        mark->setProperty("themeIcon", use[0]);
        mark->setPixmap(Theme::coloredIcon(use[0], QColor(Theme::accentLabel()), 18).pixmap(18, 18));
        body->addWidget(mark);
        body->addWidget(label(use[1], "hubSmallTitle", card));
        body->addWidget(label(use[2], "hubCardDescription", card));
        uses->addWidget(card, 1);
    }
    story->addLayout(uses);
    story->addSpacing(6);
    story->addWidget(label("Choose weights for your workflow", "hubSectionTitle", story->parentWidget()));
    story->addWidget(label("GGUF variants support local inference with llama.cpp. Higher-precision weights use more storage and memory. Compatible original weights support training workflows.", "hubBody", story->parentWidget()));
    story->addWidget(label("Published context is a capacity. A longer runtime context needs more memory for the KV cache. Use the local setup controls to inspect that trade-off for your device.", "hubCallout", story->parentWidget()));
    story->addSpacing(6);
    story->addWidget(label("Evaluate on your own tasks", "hubSectionTitle", story->parentWidget()));
    story->addWidget(label("Check the model’s license, intended uses and limitations. Test answer quality with representative prompts and documents before relying on it in your workflow.", "hubBody", story->parentWidget()));
    overviewLayout->addWidget(story->parentWidget());
    auto *snapshot = surface(overview, "Technical snapshot");
    for (const auto &entry : QList<QPair<QString, QString>>{
             {"Architecture", known(m_detailData.value("architecture").toString())}, {"Parameters", number(m_detailData.value("parameters").toDouble())},
             {"Task", taskName(m_detailData)}, {"Max. context", tokens(m_detailData.value("context_length"))},
             {"Languages", strings(m_detailData.value("languages"))}, {"Weight formats", strings(m_detailData.value("formats"))}})
        addSpec(snapshot, entry.first, entry.second);
    overviewLayout->addWidget(snapshot->parentWidget());
    overviewLayout->addStretch();
    m_tabs->addTab(overview, "Overview");

    auto *specifications = new QWidget(m_tabs);
    auto *specLayout = new QVBoxLayout(specifications);
    specLayout->setContentsMargins(0, 18, 0, 0);
    auto *specs = surface(specifications, "Model specifications");
    for (const auto &entry : QList<QPair<QString, QString>>{
             {"Source repository", m_detailRepo}, {"Revision", known(m_detailData.value("revision").toString())},
             {"Architecture", known(m_detailData.value("architecture").toString())},
             {"Task", taskName(m_detailData)}, {"Library", known(m_detailData.value("library_name").toString())},
             {"Languages", strings(m_detailData.value("languages"))}, {"Layers", number(m_detailData.value("layers").toDouble())},
             {"Hidden size", number(m_detailData.value("hidden_size").toDouble())},
             {"Attention heads", number(m_detailData.value("attention_heads").toDouble())}, {"KV heads", number(m_detailData.value("kv_heads").toDouble())}})
        addSpec(specs, entry.first, entry.second);
    specLayout->addWidget(specs->parentWidget());
    specLayout->addStretch();
    m_tabs->addTab(specifications, "Specifications");

    const auto inventory = m_detailData.value("files").toArray();
    auto *files = new QWidget(m_tabs);
    auto *fileLayout = new QVBoxLayout(files);
    fileLayout->setContentsMargins(0, 18, 0, 0);
    auto *fileBody = surface(files, "Files and weight variants");
    fileBody->addWidget(label(QString("%1 of %2 repository files shown. The download action includes the complete repository.").arg(inventory.size()).arg(m_detailData.value("file_count").toInt()), "hubMeta", fileBody->parentWidget()));
    auto *fileTable = table({"File", "Size"}, fileBody->parentWidget());
    fileTable->setObjectName("hubFileTable");
    fileTable->setRowCount(inventory.size());
    fileTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    fileTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    for (int row = 0; row < inventory.size(); ++row) {
        fileTable->setItem(row, 0, new QTableWidgetItem(inventory[row].toObject().value("path").toString()));
        fileTable->setItem(row, 1, new QTableWidgetItem(bytes(inventory[row].toObject().value("size_bytes"))));
    }
    fileBody->addWidget(fileTable, 1);
    fileLayout->addWidget(fileBody->parentWidget());
    fileLayout->addStretch();
    m_tabs->addTab(files, "Files && variants");
    auto *evaluations = new QWidget(m_tabs);
    auto *evaluationLayout = new QVBoxLayout(evaluations);
    evaluationLayout->setContentsMargins(0, 18, 0, 0);
    auto *evaluationBody = surface(evaluations, "Evaluation snapshot");
    const auto metrics = m_detailData.value("evaluations").toArray();
    evaluationBody->addWidget(label(metrics.isEmpty() ? "No structured evaluation results were reported. The publisher’s model card may contain additional evaluation context." : "Publisher-reported results. Review dataset, method, prompt format and weight precision before comparing scores.", "hubBody", evaluationBody->parentWidget()));
    if (!metrics.isEmpty()) {
        auto *evaluationTable = table({"Dataset", "Metric", "Reported value"}, evaluationBody->parentWidget());
        evaluationTable->setObjectName("hubEvaluationTable");
        evaluationTable->setRowCount(metrics.size());
        for (int row = 0; row < metrics.size(); ++row) {
            const auto metric = metrics[row].toObject();
            evaluationTable->setItem(row, 0, new QTableWidgetItem(metric.value("dataset").toString()));
            evaluationTable->setItem(row, 1, new QTableWidgetItem(metric.value("metric").toString()));
            evaluationTable->setItem(row, 2, new QTableWidgetItem(metric.value("value").toString()));
        }
        evaluationBody->addWidget(evaluationTable, 1);
    }
    evaluationLayout->addWidget(evaluationBody->parentWidget());
    evaluationLayout->addStretch();
    m_tabs->addTab(evaluations, "Evaluations");
    m_tabs->setCurrentIndex(previousTab);
    columns->addWidget(m_tabs, 1);

    auto *rail = new QWidget(m_details);
    rail->setFixedWidth(292);
    auto *railLayout = new QVBoxLayout(rail);
    railLayout->setContentsMargins(0, 0, 0, 0);
    railLayout->setSpacing(18);
    auto *setup = surface(rail);
    m_memoryStatus = label("MEMORY ESTIMATE", "hubEstimateStatus", setup->parentWidget());
    setup->addWidget(label("Your local setup", "hubSectionTitle", setup->parentWidget()));
    m_detailDevice = label(known(m_telemetry.value("device").toString()), "hubSmallMeta", setup->parentWidget());
    auto *deviceRow = new QHBoxLayout;
    deviceRow->addWidget(m_detailDevice, 1);
    auto *device = new Button("Device details →", setup->parentWidget());
    device->setFlat(true);
    device->setObjectName("hubTextLink");
    device->setMinimumWidth(120);
    connect(device, &QPushButton::clicked, this, &ModelHubPanel::showDeviceDetails);
    deviceRow->addWidget(device);
    setup->addLayout(deviceRow);
    m_detailStatus = label(m_detailLoading ? "Loading publisher metadata…" : m_detailError, "hubMeta", setup->parentWidget());
    m_detailStatus->setVisible(m_detailLoading || !m_detailError.isEmpty());
    setup->addWidget(m_detailStatus);
    if (!m_detailError.isEmpty()) {
        auto *retry = new Button("Retry metadata", setup->parentWidget());
        retry->setObjectName("hubRetryMetadata");
        connect(retry, &QPushButton::clicked, this, [this] { openDetails(m_detailRepo); });
        setup->addWidget(retry);
    }
    m_weight = new ComboBox(setup->parentWidget());
    m_weight->setObjectName("hubEstimateWeight");
    m_weight->addItem("FP16 parameter estimate", -1.0);
    for (const auto &entry : inventory) {
        const auto file = entry.toObject();
        const auto path = file.value("path").toString();
        if (path.endsWith(".gguf", Qt::CaseInsensitive) && !path.contains(QRegularExpression("-\\d{5}-of-\\d{5}"))) {
            const auto quant = QRegularExpression("(Q[2-8]_[A-Za-z0-9_]+)").match(path);
            const QString title = quant.hasMatch() ? quant.captured(1) + " · " + bytes(file.value("size_bytes")) : path;
            m_weight->addItem(title, file.value("size_bytes").toDouble(-1));
            m_weight->setItemData(m_weight->count() - 1, path, Qt::ToolTipRole);
        }
    }
    const int weightIndex = m_weight->findText(previousWeight);
    if (weightIndex >= 0 && (hadWeightChoices || m_weight->count() == 1)) m_weight->setCurrentIndex(weightIndex);
    else if (m_weight->count() > 1) m_weight->setCurrentIndex(1);
    Surface::field(setup, "Weight variant for estimate", m_weight);
    m_context = new ComboBox(setup->parentWidget());
    m_context->setObjectName("hubEstimateContext");
    const int maximum = m_detailData.value("context_length").toInt();
    for (const int context : {2048, 4096, 8192, 16384, 32768})
        if (maximum <= 0 || context <= maximum) m_context->addItem(QString::number(context / 1024) + "K tokens", context);
    if (m_context->count() == 0 && maximum > 0) m_context->addItem(QString::number(maximum) + " tokens", maximum);
    const int contextIndex = m_context->findData(previousContext);
    m_context->setCurrentIndex(contextIndex >= 0 ? contextIndex : std::min(1, m_context->count() - 1));
    Surface::field(setup, "Context for estimate", m_context);
    setup->addSpacing(2);
    setup->addWidget(label("Estimated weights + KV cache", "hubSmallMeta", setup->parentWidget()));
    m_estimate = label({}, "hubMemoryValue", setup->parentWidget());
    m_estimate->setObjectName("hubMemoryEstimate");
    setup->addWidget(m_estimate);
    m_memoryBar = new MemoryBreakdown(setup->parentWidget());
    setup->addWidget(m_memoryBar);
    auto *legend = new QHBoxLayout;
    legend->addWidget(label("■ Weights", "hubWeightKey", setup->parentWidget()));
    legend->addWidget(label("■ KV cache", "hubCacheKey", setup->parentWidget()));
    setup->addLayout(legend);
    m_estimateDetail = label({}, "hubSmallMeta", setup->parentWidget());
    setup->addWidget(m_memoryStatus);
    setup->addWidget(m_estimateDetail);
    setup->addWidget(label("Estimates are advisory. Download includes all repository files; configure loading in Conversation.", "hubSmallMeta", setup->parentWidget()));
    connect(m_detailAction, &QPushButton::clicked, this, [this] {
        if (m_detailData.value("downloaded").toBool()) emit loadRequested(localId(m_detailRepo));
        else startDownload();
    });
    connect(m_weight, &QComboBox::currentIndexChanged, this, &ModelHubPanel::updateEstimate);
    connect(m_context, &QComboBox::currentIndexChanged, this, &ModelHubPanel::updateEstimate);
    railLayout->addWidget(setup->parentWidget());
    auto *source = surface(rail, "Provenance and access");
    addSpec(source, "Publisher", m_detailRepo.section('/', 0, 0));
    const QString revision = m_detailData.value("revision").toString();
    addSpec(source, "Revision", revision.isEmpty() ? "Not reported" : revision.left(12));
    addSpec(source, "Access", m_detailData.value("gated").toBool() ? "Approval required" : "Public repository");
    auto *modelCard = new Button("Open publisher’s model card ↗", source->parentWidget());
    modelCard->setFlat(true);
    modelCard->setObjectName("hubTextLink");
    connect(modelCard, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(publisherUrl(m_detailRepo)); });
    source->addWidget(modelCard);
    auto *copy = new Button("Copy repository ID", source->parentWidget());
    copy->setFlat(true);
    copy->setObjectName("hubTextLink");
    connect(copy, &QPushButton::clicked, this, [this] { QApplication::clipboard()->setText(m_detailRepo); status("Repository ID copied."); });
    source->addWidget(copy);
    railLayout->addWidget(source->parentWidget());
    railLayout->addStretch();
    columns->addWidget(rail, 0, Qt::AlignTop);
    m_detailLayout->addLayout(columns);
    m_detailLayout->addStretch();
    updateEstimate();
    updateDownloadUi();
}

void ModelHubPanel::backToDiscovery() {
    ++m_detailGeneration;
    status({});
    m_pages->setCurrentIndex(0);
    m_detailRepo.clear();
    m_detailAction = nullptr;
    m_detailStatus = nullptr;
    m_weight = nullptr;
    m_context = nullptr;
    m_estimate = nullptr;
    m_estimateDetail = nullptr;
    m_memoryBar = nullptr;
    m_memoryStatus = nullptr;
    m_detailDevice = nullptr;
    renderCards();
    m_search->setFocus();
}

void ModelHubPanel::updateEstimate() {
    if (!m_estimate || !m_weight || !m_context || !m_estimateDetail || !m_memoryBar) return;
    const double parameters = m_detailData.value("parameters").toDouble();
    double weightBytes = m_weight->currentData().toDouble();
    const bool fp16 = m_weight->currentIndex() == 0;
    if (fp16 && parameters > 0) weightBytes = parameters * 2;
    auto *bar = static_cast<MemoryBreakdown *>(m_memoryBar);
    if (weightBytes <= 0) {
        m_estimate->setText("Not available");
        m_estimateDetail->setText("The publisher has not reported a usable weight size or parameter count.");
        m_memoryStatus->setText("MEMORY ESTIMATE");
        bar->setValues(0, 0, 0);
        return;
    }
    const double layers = m_detailData.value("layers").toDouble();
    const double heads = m_detailData.value("attention_heads").toDouble();
    const double kvHeads = m_detailData.value("kv_heads").toDouble(heads);
    const double hidden = m_detailData.value("hidden_size").toDouble();
    const double dimension = m_detailData.value("head_dim").toDouble(heads > 0 ? hidden / heads : 0);
    const bool cacheKnown = layers > 0 && kvHeads > 0 && dimension > 0;
    const double cache = cacheKnown ? 2 * 2 * layers * kvHeads * dimension * m_context->currentData().toDouble() : 0;
    const auto total = static_cast<qint64>(weightBytes + cache);
    const double capacity = m_telemetry.value("total_bytes").toDouble();
    m_estimate->setText((cacheKnown ? "" : "≥ ") + ModelCard::formatBytes(total));
    QString explanation = fp16 ? "FP16 weight assumption. " : "Selected weight file. ";
    explanation += cacheKnown ? "Includes an estimated FP16 KV cache for one sequence. Allow additional runtime memory." : "KV cache is unknown. Allow additional cache and runtime memory.";
    const double available = capacity - m_telemetry.value("used_bytes").toDouble();
    QString state = "MEMORY ESTIMATE";
    if (cacheKnown && available > 0 && m_telemetry.value("accelerator").toString() == "CUDA") {
        state = total > available ? "Exceeds available GPU memory" : "Within GPU budget before overhead";
    }
    m_memoryStatus->setText(state);
    m_memoryStatus->setProperty("warning", state.startsWith("Exceeds"));
    m_memoryStatus->setProperty("state", state.startsWith("Within") ? "ok" : "");
    m_memoryStatus->style()->unpolish(m_memoryStatus);
    m_memoryStatus->style()->polish(m_memoryStatus);
    m_estimateDetail->setText(explanation);
    bar->setValues(weightBytes, cache, capacity);
    bar->setAccessibleDescription(QString("Weights %1, KV cache %2, device capacity %3").arg(ModelCard::formatBytes(static_cast<qint64>(weightBytes)), cacheKnown ? ModelCard::formatBytes(static_cast<qint64>(cache)) : "unknown", bytes(m_telemetry.value("total_bytes"))));
}

void ModelHubPanel::updateDeviceUi() {
    const bool measured = !m_telemetry.isEmpty();
    m_device->setText(measured ? known(m_telemetry.value("device").toString()) + " · " + known(m_telemetry.value("accelerator").toString()) : m_deviceLoading ? "Reading your hardware…" : "Hardware readings unavailable");
    const double total = m_telemetry.value("total_bytes").toDouble();
    const bool hasMemory = total > 0 && m_telemetry.value("used_bytes").isDouble();
    m_deviceUsage->setVisible(hasMemory);
    if (hasMemory) {
        const double used = m_telemetry.value("used_bytes").toDouble();
        m_deviceUsage->setValue(qBound(0, qRound(used / total * 100), 100));
        m_deviceMeta->setText(QString("%1 / %2 %3 in use").arg(bytes(m_telemetry.value("used_bytes")), bytes(m_telemetry.value("total_bytes")), m_telemetry.value("kind").toString().toUpper()));
    } else m_deviceMeta->setText(m_deviceLoading ? "Updating memory readings…" : "Memory measurements unavailable");
    m_deviceButton->setEnabled(true);
    if (m_detailDevice) m_detailDevice->setText(measured ? known(m_telemetry.value("device").toString()) + "\n" + bytes(m_telemetry.value("total_bytes")) + " " + m_telemetry.value("kind").toString() : "Device measurements unavailable");
    updateEstimate();
    emit deviceUpdated();
}

void ModelHubPanel::refreshDevice() {
    if (m_deviceLoading) return;
    if (!m_client || !m_client->isReady()) {
        m_deviceError = "Connect to the backend to read device information.";
        updateDeviceUi();
        return;
    }
    m_deviceLoading = true;
    m_deviceError.clear();
    updateDeviceUi();
    request("/training/telemetry", [this](int code, const QJsonDocument &document) {
        m_deviceLoading = false;
        if (code != 200 || !document.isObject()) {
            m_telemetry = {};
            m_deviceError = "Device readings could not be refreshed. Try again.";
        } else m_telemetry = document.object();
        updateDeviceUi();
    });
}

void ModelHubPanel::showDeviceDetails() {
    SettingsDetailDialog dialog(this);
    dialog.setObjectName("hubDeviceDialog");
    dialog.setWindowTitle("Your device");
    dialog.resize(580, 620);
    dialog.setMinimumWidth(480);
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 22, 24, 20);
    layout->setSpacing(16);
    dialogHeading(layout, "Your device", &dialog);
    layout->addWidget(label("Current accelerator and memory readings from your machine.", "hubMeta", &dialog));
    auto *statusText = label({}, "hubSmallMeta", &dialog);
    statusText->setObjectName("hubDeviceReadingsStatus");
    layout->addWidget(statusText);
    auto *body = new QWidget(&dialog);
    auto *fields = new QVBoxLayout(body);
    fields->setContentsMargins(0, 0, 0, 0);
    fields->setSpacing(0);
    layout->addWidget(scrollPage(body, &dialog), 1);
    auto *actions = new QHBoxLayout;
    auto *refresh = new Button("Refresh readings", &dialog);
    refresh->setObjectName("hubRefreshDevice");
    auto *close = new Button("Close", &dialog);
    close->setObjectName("hubCloseDevice");
    actions->addWidget(refresh);
    actions->addStretch();
    actions->addWidget(close);
    layout->addLayout(actions);
    auto render = [this, fields, refresh, statusText] {
        clearLayout(fields);
        refresh->setEnabled(!m_deviceLoading && m_client && m_client->isReady());
        statusText->setText(m_deviceLoading ? "Refreshing readings…" : m_deviceError.isEmpty() ? "Device-wide readings include memory used by other processes." : m_deviceError);
        const double total = m_telemetry.value("total_bytes").toDouble();
        const double used = m_telemetry.value("used_bytes").toDouble();
        auto scalar = [this](const QString &key, const QString &suffix) {
            const auto value = m_telemetry.value(key);
            return value.isDouble() ? QString::number(value.toDouble(), 'f', 0) + suffix : QString("Not reported");
        };
        for (const auto &entry : QList<QPair<QString, QString>>{
                 {"Accelerator", known(m_telemetry.value("device").toString())},
                 {"Runtime", known(m_telemetry.value("accelerator").toString())},
                 {"Memory type", known(m_telemetry.value("kind").toString())},
                 {"Total memory", bytes(m_telemetry.value("total_bytes"))},
                 {"Memory in use", bytes(m_telemetry.value("used_bytes"))},
                 {"Available memory", total > 0 && m_telemetry.value("used_bytes").isDouble() ? ModelCard::formatBytes(static_cast<qint64>(std::max(0.0, total - used))) : "Not reported"},
                 {"Logical CPU threads", scalar("cpu_count", "")}, {"GPU utilization", scalar("utilization_percent", "%")},
                 {"CPU utilization", scalar("cpu_percent", "%")}, {"Temperature", scalar("temperature_c", " °C")}})
            addSpec(fields, entry.first, entry.second);
        fields->addStretch();
    };
    connect(this, &ModelHubPanel::deviceUpdated, &dialog, render);
    connect(refresh, &QPushButton::clicked, this, &ModelHubPanel::refreshDevice);
    connect(close, &QPushButton::clicked, &dialog, &QDialog::reject);
    render();
    refreshDevice();
    dialog.exec();
}

void ModelHubPanel::startDownload() {
    if (!m_pullRepo.isEmpty()) { status("A model download is already running. Cancel it or wait for completion."); return; }
    if (m_detailRepo.isEmpty() || !m_client || !m_client->isReady()) return;
    const QString message = QString("Download the complete repository %1?\n\nRepository size: %2\nAll repository files will be downloaded to the shared Hugging Face cache.\nReview the publisher’s license and access terms before downloading.")
        .arg(m_detailRepo, bytes(m_detailData.value("total_bytes")));
    SettingsDetailDialog confirmation(this);
    confirmation.setObjectName("hubDownloadDialog");
    confirmation.setWindowTitle("Download repository");
    confirmation.resize(520, 300);
    auto *layout = new QVBoxLayout(&confirmation);
    layout->setContentsMargins(24, 22, 24, 20);
    layout->setSpacing(18);
    dialogHeading(layout, "Download repository", &confirmation);
    layout->addWidget(label(message, "hubBody", &confirmation), 1);
    auto *actions = new QHBoxLayout;
    actions->addStretch();
    auto *cancel = new Button("Cancel", &confirmation);
    cancel->setDefault(true);
    auto *download = new Button("Download repository", &confirmation);
    download->setObjectName("hubConfirmDownload");
    download->setProperty("primary", true);
    download->setAutoDefault(false);
    actions->addWidget(cancel);
    actions->addWidget(download);
    layout->addLayout(actions);
    connect(cancel, &QPushButton::clicked, &confirmation, &QDialog::reject);
    connect(download, &QPushButton::clicked, &confirmation, &QDialog::accept);
    cancel->setFocus();
    if (confirmation.exec() != QDialog::Accepted) return;
    m_pullRepo = m_detailRepo;
    m_pullJob.clear();
    m_cancelling = false;
    m_pullProgress = {};
    updateDownloadUi();
    const auto repo = m_pullRepo;
    QPointer<ModelHubPanel> guard(this);
    m_client->post("/models/pull", QJsonObject{{"ref", repo}, {"source", "huggingface"}}, [guard, repo](int code, const QJsonDocument &document) {
        if (!guard || guard->m_pullRepo != repo) return;
        if (code != 202 || document.object().value("id").toString().isEmpty()) {
            guard->finishDownload("Could not start download. " + document.object().value("detail").toString(), false);
            return;
        }
        guard->m_pullJob = document.object().value("id").toString();
        guard->m_pullTimer->start();
        if (guard->m_cancelling) guard->sendCancel();
        guard->pollDownload();
    });
}

void ModelHubPanel::cancelDownload() {
    if (m_pullRepo.isEmpty() || m_cancelling) return;
    m_cancelling = true;
    updateDownloadUi();
    if (!m_pullJob.isEmpty()) sendCancel();
}

void ModelHubPanel::sendCancel() {
    if (!m_client || !m_client->isReady()) { m_cancelling = false; updateDownloadUi(); return; }
    const auto job = m_pullJob;
    QPointer<ModelHubPanel> guard(this);
    m_client->post("/models/pull/" + job + "/cancel", {}, [guard, job](int code, const QJsonDocument &document) {
        if (!guard || job != guard->m_pullJob) return;
        if (code == 200 || code == 202) return;
        guard->m_cancelling = false;
        guard->updateDownloadUi();
        guard->status("Could not cancel download. " + document.object().value("detail").toString(), "error");
    });
}

void ModelHubPanel::pollDownload() {
    if (m_pullJob.isEmpty() || m_polling || !m_client || !m_client->isReady()) return;
    m_polling = true;
    const auto job = m_pullJob;
    request("/models/pull/" + job, [this, job](int code, const QJsonDocument &document) {
        if (job != m_pullJob) return;
        m_polling = false;
        if (code == 404) {
            finishDownload("Download job is no longer available. Refresh Library before retrying.", false);
            return;
        }
        if (code != 200) {
            status("Download status unavailable. Retrying; Cancel remains available.", "warn");
            return;
        }
        m_pullProgress = document.object();
        const auto value = m_pullProgress.value("status").toString();
        if (value == "done") finishDownload("Model downloaded and available in Library.", true);
        else if (value == "cancelled") finishDownload("Download cancelled. Partial files were removed.", false);
        else if (value == "error") finishDownload("Download failed. " + m_pullProgress.value("error").toString(), false);
        else updateDownloadUi();
    });
}

void ModelHubPanel::finishDownload(const QString &message, bool success) {
    const auto repo = m_pullRepo;
    m_pullTimer->stop();
    m_pullJob.clear();
    m_pullRepo.clear();
    m_polling = false;
    m_cancelling = false;
    if (success) {
        for (auto &entry : m_models) if (repoId(entry) == repo) entry["downloaded"] = true;
        if (m_metadata.contains(repo)) m_metadata[repo]["downloaded"] = true;
        if (m_detailRepo == repo) m_detailData["downloaded"] = true;
        emit modelsChanged();
        emit downloadFinished(true);
    } else if (message.startsWith("Download failed") || message.startsWith("Could not start")) emit downloadFinished(false);
    emit downloadActivity(repo, success ? "Completed" : message.startsWith("Download cancelled") ? "Cancelled" : "Failed", message);
    status(message, success ? "ok" : QString());
    updateDownloadUi();
    if (m_pages->currentIndex() == 0) renderCards();
}

void ModelHubPanel::updateDownloadUi() {
    m_downloadStrip->setVisible(!m_pullRepo.isEmpty());
    const int progress = qBound(0, static_cast<int>(m_pullProgress.value("progress").toDouble()), 100);
    m_downloadText->setText(m_cancelling ? "Cancelling " + m_pullRepo + "…" : QString("%1 · %2% · %3 / %4").arg(m_pullRepo).arg(progress).arg(bytes(m_pullProgress.value("bytes_done")), bytes(m_pullProgress.value("bytes_total"))));
    m_progress->setValue(progress);
    m_cancelButton->setEnabled(!m_cancelling);
    m_cancelButton->setText(m_cancelling ? "Cancelling…" : "Cancel download");
    if (m_detailAction) {
        const bool local = m_detailData.value("downloaded").toBool();
        m_detailAction->setText(local ? "Load in Conversation" : "Download repository");
        m_detailAction->setEnabled(local || (m_pullRepo.isEmpty() && !m_detailLoading && m_detailError.isEmpty()));
    }
}

void ModelHubPanel::status(const QString &message, const QString &tone) {
    m_status->setText(message);
    m_status->setVisible(!message.isEmpty());
    applyStatusTone(m_status, tone);
}

QString ModelHubPanel::settingsJson() const {
    return QString::fromUtf8(QJsonDocument(QJsonObject{{"lastSearch", m_search->text()}}).toJson(QJsonDocument::Compact));
}

void ModelHubPanel::applySettingsJson(const QString &json) {
    const auto document = QJsonDocument::fromJson(json.toUtf8());
    if (document.isObject()) m_search->setText(document.object().value("lastSearch").toString());
}
