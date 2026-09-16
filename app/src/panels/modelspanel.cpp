#include "panels/modelspanel.h"

#include "backend_client.h"
#include "panels/modelcard.h"
#include "widgets/flowlayout.h"

#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

namespace {
constexpr int kPullPollMs = 400;
}

ModelsPanel::ModelsPanel(QWidget *parent)
    : BasePanel("Models", parent)
    , m_client(nullptr)
    , m_searchEdit(nullptr)
    , m_searchButton(nullptr)
    , m_refreshButton(nullptr)
    , m_downloadButton(nullptr)
    , m_deleteButton(nullptr)
    , m_viewCombo(nullptr)
    , m_scrollArea(nullptr)
    , m_flowLayout(nullptr)
    , m_cardHost(nullptr)
    , m_statusLabel(nullptr)
    , m_pullTimer(nullptr)
    , m_selectedCard(nullptr)
{
    setHeaderSubtitle(
        "Browse and download models from HuggingFace, then run them locally.");
    setupUi();
}

QWidget *ModelsPanel::makeCardHost() {
    m_cardHost = new QWidget(this);
    m_flowLayout = new FlowLayout(m_cardHost, 12, 12);
    m_flowLayout->setContentsMargins(2, 2, 2, 2);
    m_cardHost->setLayout(m_flowLayout);
    return m_cardHost;
}

void ModelsPanel::setupUi() {
    auto *layout = contentLayout();

    auto *searchRow = new QHBoxLayout();
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText("Search HuggingFace models...");
    m_searchButton = new QPushButton("Search", this);
    m_searchButton->setObjectName("primaryBtn");
    m_refreshButton = new QPushButton("Refresh", this);
    m_refreshButton->setObjectName("secondaryBtn");
    searchRow->addWidget(m_searchEdit, 1);
    searchRow->addWidget(m_searchButton);
    searchRow->addWidget(m_refreshButton);
    layout->addLayout(searchRow);

    auto *viewRow = new QHBoxLayout();
    m_viewCombo = new QComboBox(this);
    m_viewCombo->addItems({"Local Models", "Hub Search Results"});
    m_downloadButton = new QPushButton("Download", this);
    m_downloadButton->setObjectName("primaryBtn");
    m_deleteButton = new QPushButton("Delete", this);
    m_deleteButton->setObjectName("destructiveBtn");
    m_downloadButton->setEnabled(false);
    m_deleteButton->setEnabled(false);
    viewRow->addWidget(m_viewCombo);
    viewRow->addSpacing(8);
    viewRow->addWidget(m_downloadButton);
    viewRow->addWidget(m_deleteButton);
    viewRow->addStretch();
    layout->addLayout(viewRow);

    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setObjectName("cardScroll");
    // The viewport would otherwise paint the default light palette; let the
    // flat-black app root show through between the cards.
    m_scrollArea->viewport()->setAutoFillBackground(false);
    m_scrollArea->setWidget(makeCardHost());
    layout->addWidget(m_scrollArea, 1);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName("statusLabel");
    layout->addWidget(m_statusLabel);

    connect(m_searchButton, &QPushButton::clicked, this, &ModelsPanel::onSearch);
    connect(m_refreshButton, &QPushButton::clicked, this, &ModelsPanel::refresh);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &ModelsPanel::onSearch);
    connect(m_viewCombo, &QComboBox::currentIndexChanged, this, &ModelsPanel::onViewChanged);
    connect(m_downloadButton, &QPushButton::clicked, this, &ModelsPanel::onDownload);
    connect(m_deleteButton, &QPushButton::clicked, this, &ModelsPanel::onDeleteModel);

    m_pullTimer = new QTimer(this);
    m_pullTimer->setInterval(kPullPollMs);
    connect(m_pullTimer, &QTimer::timeout, this, &ModelsPanel::onPullPoll);

    switchToLocalView();
    setStatus("Backend not connected yet. The list will populate when ready.");
}

void ModelsPanel::setBackendClient(BackendClient *client) {
    m_client = client;
    setStatus(client && client->isReady()
                  ? "Ready. Loading local models..."
                  : "Backend not connected.");
    refresh();
}

bool ModelsPanel::isLocalView() const {
    return m_viewCombo->currentIndex() == 0;
}

void ModelsPanel::switchToLocalView() {
    m_viewCombo->setCurrentIndex(0);
}

void ModelsPanel::refresh() {
    if (!m_client || !m_client->isReady()) {
        setStatus("Backend not connected.");
        return;
    }
    if (isLocalView()) {
        m_client->get("/models", [this](int status, const QJsonDocument &doc) {
            if (status != 200) {
                setStatus(QString("Failed to load models (HTTP %1).").arg(status), "error");
                return;
            }
            showLocalModels(doc.array());
        });
    }
}

void ModelsPanel::onViewChanged(int index) {
    Q_UNUSED(index);
    clearCards();
    updateActionButtons();
    if (isLocalView()) {
        m_refreshButton->setEnabled(true);
        m_searchButton->setEnabled(true);
        refresh();
    } else {
        m_refreshButton->setEnabled(false);
        setStatus("Enter a search term and press Enter or click Search.");
    }
}

void ModelsPanel::clearCards() {
    while (QLayoutItem *item = m_flowLayout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->hide();            // stop painting immediately
            w->setParent(nullptr); // detach from host so it can't overlap new cards
            w->deleteLater();
        }
        delete item;
    }
    m_cards.clear();
    if (m_selectedCard) {
        m_selectedCard = nullptr;
    }
    m_flowLayout->invalidate();
    m_cardHost->updateGeometry();
}

void ModelsPanel::addCards(const QJsonArray &items, bool isHub) {
    for (const auto &value : items) {
        auto *card = new ModelCard(value.toObject(), isHub, m_cardHost);
        connect(card, &ModelCard::cardClicked, this, &ModelsPanel::onCardClicked);
        m_flowLayout->addWidget(card);
        m_cards.append(card);
    }
    m_flowLayout->invalidate();
    m_cardHost->updateGeometry();
}

void ModelsPanel::showLocalModels(const QJsonArray &models) {
    clearCards();
    addCards(models, false);
    setStatus(models.isEmpty()
                  ? "No local models yet. Search the Hub and download one."
                  : QString("%1 local model(s).").arg(models.size()));
    updateActionButtons();
    emit modelsChanged();
}

void ModelsPanel::onSearch() {
    const QString query = m_searchEdit->text().trimmed();
    if (query.isEmpty()) {
        setStatus("Enter a search term.");
        return;
    }
    if (!m_client || !m_client->isReady()) {
        setStatus("Backend not connected.");
        return;
    }
    m_lastSearch = query;
    m_viewCombo->setCurrentIndex(1);
    setStatus(QString("Searching Hub for \"%1\"...").arg(query));
    QUrlQuery q;
    q.addQueryItem("q", query);
    q.addQueryItem("limit", "25");
    m_client->get("/models/search?" + q.toString(QUrl::FullyEncoded),
                  [this](int status, const QJsonDocument &doc) {
        if (status != 200) {
            setStatus(QString("Search failed (HTTP %1).").arg(status), "error");
            return;
        }
        showSearchResults(doc.array());
    });
}

void ModelsPanel::showSearchResults(const QJsonArray &results) {
    clearCards();
    addCards(results, true);
    setStatus(results.isEmpty()
                  ? "No results. Try a different search term."
                  : QString("%1 result(s). Select one and click Download.")
                        .arg(results.size()));
    updateActionButtons();
}

void ModelsPanel::onCardClicked(ModelCard *card) {
    if (m_selectedCard == card) return;
    if (m_selectedCard)
        m_selectedCard->setSelected(false);
    m_selectedCard = card;
    card->setSelected(true);
    updateActionButtons();
}

void ModelsPanel::updateActionButtons() {
    const bool hasSelection = m_selectedCard != nullptr;
    m_downloadButton->setEnabled(hasSelection && !isLocalView());
    m_deleteButton->setEnabled(hasSelection && isLocalView());
}

QString ModelsPanel::selectedId() const {
    return m_selectedCard ? m_selectedCard->id() : QString();
}

ModelCard *ModelsPanel::cardById(const QString &id) const {
    for (ModelCard *card : m_cards) {
        if (card && card->id() == id)
            return card;
    }
    return nullptr;
}

void ModelsPanel::onDownload() {
    const QString ref = selectedId();
    if (ref.isEmpty() || !m_client || !m_client->isReady()) return;

    setStatus(QString("Downloading \"%1\"...").arg(ref));
    m_downloadButton->setEnabled(false);
    m_downloadingId = ref;
    if (ModelCard *card = cardById(ref))
        card->setProgress(0);

    QJsonObject body;
    body["ref"] = ref;
    body["source"] = "huggingface";
    m_client->post("/models/pull", body, [this](int status, const QJsonDocument &doc) {
        if (status != 202) {
            setStatus(QString("Could not start download (HTTP %1).").arg(status), "error");
            return;
        }
        startPullPolling(doc.object().value("id").toString());
    });
}

void ModelsPanel::startPullPolling(const QString &jobId) {
    m_pullJobId = jobId;
    onPullPoll();
    m_pullTimer->start();
}

void ModelsPanel::onPullPoll() {
    if (m_pullJobId.isEmpty() || !m_client || !m_client->isReady()) {
        m_pullTimer->stop();
        return;
    }
    m_client->get("/models/pull/" + m_pullJobId,
                  [this](int status, const QJsonDocument &doc) {
        if (status != 200) {
            m_pullTimer->stop();
            m_pullJobId.clear();
            setStatus(QString("Lost track of download job (HTTP %1).").arg(status), "error");
            return;
        }
        const QJsonObject job = doc.object();
        const QString jobStatus = job.value("status").toString();
        const double progress = job.value("progress").toDouble();
        const QString message = job.value("message").toString();

        if (jobStatus == "done") {
            m_pullTimer->stop();
            m_pullJobId.clear();
            if (ModelCard *card = cardById(m_downloadingId))
                card->setProgress(-1);
            m_downloadingId.clear();
            setStatus("Download complete. Refreshing local models...", "ok");
            switchToLocalView();
            refresh();
        } else if (jobStatus == "error") {
            m_pullTimer->stop();
            m_pullJobId.clear();
            if (ModelCard *card = cardById(m_downloadingId))
                card->setProgress(-1);
            m_downloadingId.clear();
            setStatus("Download failed: " + job.value("error").toString(), "error");
        } else {
            if (ModelCard *card = cardById(m_downloadingId))
                card->setProgress(progress);
            setStatus(QString("Downloading: %1%  (%2)")
                          .arg(static_cast<int>(progress))
                          .arg(message));
        }
    });
}

void ModelsPanel::onDeleteModel() {
    const QString id = selectedId();
    if (id.isEmpty() || !m_client || !m_client->isReady()) return;

    m_client->remove("/models/" + id, [this](int status, const QJsonDocument &doc) {
        Q_UNUSED(doc);
        if (status == 204) {
            setStatus("Model deleted.");
            refresh();
        } else if (status == 404) {
            setStatus("Model not found (already removed?).");
            refresh();
        } else {
            setStatus(QString("Delete failed (HTTP %1).").arg(status), "error");
        }
    });
}

void ModelsPanel::setStatus(const QString &text, const QString &tone) {
    m_statusLabel->setText(text);
    applyStatusTone(m_statusLabel, tone);
}

QString ModelsPanel::settingsJson() const {
    QJsonObject obj;
    obj["lastSearch"] = m_searchEdit->text();
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void ModelsPanel::applySettingsJson(const QString &json) {
    QJsonParseError err;
    auto doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return;
    m_searchEdit->setText(doc.object().value("lastSearch").toString());
}