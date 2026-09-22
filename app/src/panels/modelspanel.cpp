#include "panels/modelspanel.h"

#include "backend_client.h"
#include "panels/modelcard.h"
#include "widgets/flowlayout.h"
#include "widgets/overviewtile.h"
#include "widgets/surface.h"
#include "widgets/starfield.h"

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
    , m_overviewTimer(nullptr)
    , m_selectedCard(nullptr)
{
    setHeaderSubtitle(
        "Local weights and remote repositories. Select a model to begin.");
    setupUi();
}

QWidget *ModelsPanel::makeCardHost() {
    m_cardHost = new QWidget(this);
    m_flowLayout = new FlowLayout(m_cardHost, 24, 24);
    m_flowLayout->setContentsMargins(2, 2, 2, 2);
    m_cardHost->setLayout(m_flowLayout);
    return m_cardHost;
}

void ModelsPanel::setupUi() {
    auto *layout = contentLayout();
    layout->setAlignment(Qt::AlignTop);

    auto *note = new QFrame(this);
    note->setObjectName("studioNote");
    auto *noteRow = new QHBoxLayout(note);
    noteRow->setContentsMargins(12,8,12,8);
    auto *mark = new Starfield(note);
    mark->setProperty("kineticGlyph",true);
    mark->setFixedSize(34,34);
    noteRow->addWidget(mark);
    auto *noteTitle = new QLabel("Independent thinking.",note);
    noteTitle->setObjectName("noteTitle");
    noteRow->addWidget(noteTitle);
    noteRow->addStretch();
    auto *noteCaption = new QLabel("Your models. Your data. Your machine.",note);
    noteCaption->setObjectName("cardMeta");
    noteCaption->setWordWrap(false);
    noteRow->addWidget(noteCaption);
    layout->addWidget(note);

    // Live workbench overview: one state tile per workspace. Reads real
    // backend state and deep-links into each panel.
    auto *overviewRow = new QHBoxLayout();
    overviewRow->setSpacing(8);
    struct TileSpec {
        int panel;
        QString kicker;
        QString value;
        QString caption;
    };
    const TileSpec specs[] = {
        {1, "02  TRAIN", "no jobs yet", "run your first fine-tune on your data"},
        {2, "03  KNOWLEDGE", "no documents", "add notes, PDFs and pages to search"},
        {3, "04  ADAPTERS", "none active", "activate a trained adapter in chat"},
        {4, "05  PLAYGROUND", "no model loaded", "load a model and start talking"},
    };
    for (const auto &spec : specs) {
        auto *tile = new OverviewTile(spec.panel, spec.kicker, spec.value,
                                      spec.caption, this);
        connect(tile, &OverviewTile::activated,
                this, &ModelsPanel::workspaceRequested);
        overviewRow->addWidget(tile, 1);
        m_overviewTiles.append(tile);
    }
    layout->addLayout(overviewRow);

    auto *searchRow = new QHBoxLayout();
    m_searchEdit = new QLineEdit(this);
    m_searchEdit->setPlaceholderText("Search Hugging Face models…");
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setAccessibleName("Search models");
    m_searchButton = new QPushButton("Search", this);
    m_searchButton->setObjectName("primaryBtn");
    m_refreshButton = new QPushButton("Refresh", this);
    m_refreshButton->setObjectName("secondaryBtn");
    searchRow->addWidget(m_searchEdit, 1);
    searchRow->addWidget(m_searchButton);
    searchRow->addWidget(m_refreshButton);
    layout->addLayout(searchRow);

    auto *discovery = new QHBoxLayout();
    auto *discoverLabel = new QLabel("EXPLORE",this);
    discoverLabel->setObjectName("heroKicker");
    discovery->addWidget(discoverLabel);
    for (const auto &term : {QString("Qwen"),QString("Llama"),QString("Gemma"),QString("SmolLM")}) {
        auto *chip = new QPushButton(term,this);
        chip->setCursor(Qt::PointingHandCursor);
        connect(chip,&QPushButton::clicked,this,[this,term]() {
            m_searchEdit->setText(term);
            onSearch();
        });
        discovery->addWidget(chip);
    }
    discovery->addStretch();
    layout->addLayout(discovery);

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
    m_scrollArea->hide();
    m_emptyRack = new QLabel("Your library starts here\n\nSearch a repository above, select a result, then download its weights.\nLocal models appear here when the backend is connected.",this);
    m_emptyRack->setObjectName("rackEmpty");
    m_emptyRack->setAlignment(Qt::AlignCenter);
    m_emptyRack->setWordWrap(true);
    m_emptyRack->setMinimumHeight(200);
    layout->addWidget(m_emptyRack,1);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName("statusLabel");
    m_statusLabel->setWordWrap(true);
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

    // Keep the overview tiles fresh while this panel is on screen. The guards
    // inside refreshOverview() make each poll a no-op until the backend is up.
    m_overviewTimer = new QTimer(this);
    m_overviewTimer->setInterval(8000);
    connect(m_overviewTimer, &QTimer::timeout, this, [this]() {
        if (isVisible()) refreshOverview();
    });
    m_overviewTimer->start();

    switchToLocalView();
    setStatus("Backend not connected yet. The list will populate when ready.");
}

void ModelsPanel::setBackendClient(BackendClient *client) {
    m_client = client;
    setStatus(client && client->isReady()
                  ? "Ready. Loading local models..."
                  : "Backend not connected.");
    refresh();
    refreshOverview();
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
    m_scrollArea->hide();
    m_emptyRack->show();
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
    m_scrollArea->setVisible(!items.isEmpty());
    m_emptyRack->setVisible(items.isEmpty());
    m_emptyRack->setText(isHub ? "No models found\n\nTry a different repository name or model family."
                             : "Your library starts here\n\nSearch a repository above and download a model to get started.");
    for (const auto &value : items) {
        auto *card = new ModelCard(value.toObject(), isHub, m_cardHost);
        connect(card, &ModelCard::cardClicked, this, &ModelsPanel::onCardClicked);
        connect(card,&ModelCard::actionRequested,this,[this,isHub](ModelCard *selected) {
            onCardClicked(selected);
            if (isHub) onDownload(); else emit loadRequested(selected->id());
        });
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

void ModelsPanel::refreshOverview() {
    if (!m_client || !m_client->isReady()) return;
    const QStringList endpoints = {"/training/jobs", "/rag/documents", "/adapters"};
    const QStringList units = {"jobs", "documents", "adapters"};
    for (int i = 0; i < endpoints.size(); ++i) {
        m_client->get(endpoints[i], [this, i, unit = units[i]](int status, const QJsonDocument &doc) {
            if (status != 200 || !doc.isArray()) {
                m_overviewTiles[i]->setState("unavailable", "Open workspace to retry", "error");
                return;
            }
            m_overviewTiles[i]->setState(QString("%1 %2").arg(doc.array().size()).arg(unit),
                                        "Open workspace →");
        });
    }
    m_client->get("/inference/status", [this](int status, const QJsonDocument &doc) {
        if (status != 200) {
            m_overviewTiles[3]->setState("unavailable", "Open inference to retry", "error");
            return;
        }
        const auto state = doc.object();
        const QString model = state.value("loaded_model").toString();
        m_overviewTiles[3]->setState(model.isEmpty() ? "standby" : "model loaded",
                                   model.isEmpty() ? "Load a local model →" : model);
    });
}
