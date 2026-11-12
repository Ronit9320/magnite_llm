#include "widgets/confirmation.h"
#include "panels/modelspanel.h"

#include "backend_client.h"
#include "panels/modelcard.h"
#include "widgets/flowlayout.h"
#include "widgets/overviewtile.h"
#include "widgets/surface.h"
#include "widgets/studiocanvas.h"

#include "widgets/combobox.h"
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include "widgets/button.h"
#include <QScrollArea>
#include <QTimer>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>

namespace {
constexpr int kPullPollMs = 400;
constexpr int kSearchPageSize = 50;
// The backend rejects /models/search offsets above this, so paging stops here.
constexpr int kMaxSearchOffset = 1000;
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
    auto *mark = new StudioCanvas(note);
    mark->setProperty("brandMark",true);
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
    m_searchButton = new Button("Search", this);
    m_searchButton->setObjectName("primaryBtn");
    m_refreshButton = new Button("Refresh", this);
    m_refreshButton->setObjectName("secondaryBtn");
    searchRow->addWidget(m_searchEdit, 1);
    searchRow->addWidget(m_searchButton);
    searchRow->addWidget(m_refreshButton);
    layout->addLayout(searchRow);

    auto *discoveryHost = new QWidget(this);
    discoveryHost->setObjectName("libraryDiscoveryLinks");
    auto *discovery = new QHBoxLayout(discoveryHost);
    auto *discoverLabel = new QLabel("EXPLORE",this);
    discoverLabel->setObjectName("heroKicker");
    discovery->addWidget(discoverLabel);
    for (const auto &term : {QString("Qwen"),QString("Llama"),QString("Gemma"),QString("SmolLM")}) {
        auto *chip = new Button(term,this);
        chip->setCursor(Qt::PointingHandCursor);
        connect(chip,&QPushButton::clicked,this,[this,term]() {
            m_searchEdit->setText(term);
            onSearch();
        });
        discovery->addWidget(chip);
    }
    discovery->addStretch();
    layout->addWidget(discoveryHost);

    auto *viewRow = new QHBoxLayout();
    m_viewCombo = new ComboBox(this);
    m_viewCombo->addItems({"Local Models", "Hub Search Results"});
    m_downloadButton = new Button("Download", this);
    m_downloadButton->setObjectName("primaryBtn");
    m_deleteButton = new Button("Delete", this);
    m_deleteButton->setObjectName("destructiveBtn");
    m_cancelButton = new Button("Cancel download", this);
    m_cancelButton->setObjectName("cancelDownload");
    m_cancelButton->setAccessibleName("Cancel download");
    m_cancelButton->hide();
    m_downloadButton->setEnabled(false);
    m_deleteButton->setEnabled(false);
    viewRow->addWidget(m_viewCombo);
    viewRow->addSpacing(8);
    viewRow->addWidget(m_downloadButton);
    viewRow->addWidget(m_cancelButton);
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
    m_loadMoreButton = new Button("Load more results", this);
    m_loadMoreButton->setObjectName("secondaryBtn");
    m_loadMoreButton->setAccessibleName("Load more results");
    m_loadMoreButton->hide();
    layout->addWidget(m_loadMoreButton, 0, Qt::AlignHCenter);
    m_emptyRack = new QLabel("Your library starts here\n\nSearch a repository above, select a result, then download its weights.\nLocal models appear here when the backend is connected.",this);
    m_emptyRack->setObjectName("rackEmpty");
    m_emptyRack->setAlignment(Qt::AlignCenter);
    m_emptyRack->setWordWrap(true);
    m_emptyRack->setMinimumHeight(200);
    layout->addWidget(m_emptyRack,1);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName("statusLabel");
    m_statusLabel->setTextFormat(Qt::PlainText);
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);

    connect(m_searchButton, &QPushButton::clicked, this, &ModelsPanel::onSearch);
    connect(m_refreshButton, &QPushButton::clicked, this, &ModelsPanel::refresh);
    connect(m_searchEdit, &QLineEdit::returnPressed, this, &ModelsPanel::onSearch);
    connect(m_viewCombo, &QComboBox::currentIndexChanged, this, &ModelsPanel::onViewChanged);
    connect(m_downloadButton, &QPushButton::clicked, this, &ModelsPanel::onDownload);
    connect(m_deleteButton, &QPushButton::clicked, this, &ModelsPanel::onDeleteModel);
    connect(m_loadMoreButton, &QPushButton::clicked, this, &ModelsPanel::onLoadMore);
    connect(m_cancelButton, &QPushButton::clicked, this, &ModelsPanel::onCancelDownload);

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

void ModelsPanel::useLibraryWorkspace() {
    m_libraryOnly = true;
    switchToLocalView();
    m_searchEdit->hide();
    m_searchButton->hide();
    m_viewCombo->hide();
    m_downloadButton->hide();
    findChild<QWidget *>("libraryDiscoveryLinks")->hide();
    m_emptyRack->setText("Your library starts here\n\nExplore Model Hub to find and download a model. Downloaded models appear here.");
    setHeaderSubtitle("Manage downloaded weights and load models into Playground.");
    auto *browse = new Button("Explore Model Hub", this);
    connect(browse, &QPushButton::clicked, this, &ModelsPanel::hubRequested);
    contentLayout()->insertWidget(0, browse, 0, Qt::AlignRight);
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
            if (isLocalView()) showLocalModels(doc.array());
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
    m_hubIds.clear();
    m_hubOffset = 0;
    m_hasMoreResults = false;
    m_loadMoreButton->hide();
    if (m_selectedCard) {
        m_selectedCard = nullptr;
    }
    m_flowLayout->invalidate();
    m_cardHost->updateGeometry();
}

void ModelsPanel::addCards(const QJsonArray &items, bool isHub, bool append) {
    // Appending a page never changes what is on screen except for the new cards.
    if (!append) {
        m_scrollArea->setVisible(!items.isEmpty());
        m_emptyRack->setVisible(items.isEmpty());
        m_emptyRack->setText(isHub ? "No models found\n\nTry a different repository name or model family."
                                 : m_libraryOnly ? "Your library starts here\n\nExplore Model Hub to find and download a model. Downloaded models appear here."
                                                 : "Your library starts here\n\nSearch a repository above and download a model to get started.");
    }
    for (const auto &value : items) {
        auto *card = new ModelCard(value.toObject(), isHub, m_cardHost);
        connect(card, &ModelCard::cardClicked, this, &ModelsPanel::onCardClicked);
        connect(card, &ModelCard::cancelRequested, this, [this](ModelCard *) { onCancelDownload(); });
        connect(card, &ModelCard::deleteRequested, this, [this](ModelCard *selected) {
            onCardClicked(selected);
            onDeleteModel();
        });
        connect(card,&ModelCard::actionRequested,this,[this,isHub](ModelCard *selected) {
            onCardClicked(selected);
            // A Hub result already in the library loads it instead of downloading again.
            if (isHub && !selected->isDownloaded()) onDownload();
            else emit loadRequested(selected->localId());
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
    QString summary = models.isEmpty() ? QString("No local models yet. Search the Hub and download one.")
                                       : QString("%1 local model(s).").arg(models.size());
    if (!m_afterRefreshNote.isEmpty()) summary = m_afterRefreshNote + "  " + summary;
    m_afterRefreshNote.clear();
    setStatus(summary);
    updateActionButtons();
    emit modelsChanged();
}

void ModelsPanel::onSearch() {
    if (m_libraryOnly) return;
    if (m_searching) return;
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
    requestSearchPage(0);
}

void ModelsPanel::onLoadMore() {
    if (m_searching || !m_hasMoreResults || isLocalView()) return;
    if (!m_client || !m_client->isReady()) {
        setStatus("Backend not connected.", "error");
        return;
    }
    requestSearchPage(m_hubOffset);
}

// Fetch one page of results for m_lastSearch. Offset 0 replaces the shown
// results; any later offset appends to them.
void ModelsPanel::requestSearchPage(int offset) {
    const bool append = offset > 0;
    m_searching = true;
    m_searchButton->setEnabled(false);
    m_viewCombo->setEnabled(false);
    if (append) {
        m_loadMoreButton->setEnabled(false);
        m_loadMoreButton->setText("Loading…");
        setStatus(QString("Loading more results for \"%1\"...").arg(m_lastSearch));
    } else {
        m_searchButton->setText("Searching…");
        setStatus(QString("Searching Hub for \"%1\"...").arg(m_lastSearch));
    }
    QUrlQuery q;
    q.addQueryItem("q", m_lastSearch);
    q.addQueryItem("limit", QString::number(kSearchPageSize));
    q.addQueryItem("offset", QString::number(offset));
    m_client->get("/models/search?" + q.toString(QUrl::FullyEncoded),
                  [this, append](int status, const QJsonDocument &doc) {
        m_searching = false;
        m_searchButton->setText("Search");
        m_searchButton->setEnabled(true);
        m_viewCombo->setEnabled(true);
        m_loadMoreButton->setText("Load more results");
        m_loadMoreButton->setEnabled(true);
        if (status != 200) {
            // Keep whatever is already on screen so the page can be retried.
            const QString detail = doc.object().value("detail").toString();
            setStatus(detail.isEmpty() ? QString("Search failed (HTTP %1).").arg(status)
                                       : QString("Search failed: %1").arg(detail), "error");
            return;
        }
        if (isLocalView()) return;  // the user left the results while the page loaded
        if (append) appendSearchResults(doc.array());
        else showSearchResults(doc.array());
    });
}

void ModelsPanel::showSearchResults(const QJsonArray &results) {
    clearCards();
    appendSearchResults(results);
}

// Add a page of results below the existing ones, skipping any repository
// already shown (the Hub can reorder between requests).
void ModelsPanel::appendSearchResults(const QJsonArray &results) {
    const bool append = !m_cards.isEmpty();
    QJsonArray fresh;
    for (const auto &value : results) {
        const QString repo = value.toObject().value("repo_id").toString();
        if (m_hubIds.contains(repo)) continue;
        m_hubIds.insert(repo);
        fresh.append(value);
    }
    // Advance by what the server returned, not by what was new, so a page full
    // of repeats cannot stall paging.
    m_hubOffset += results.size();
    m_hasMoreResults = results.size() >= kSearchPageSize && m_hubOffset <= kMaxSearchOffset;
    addCards(fresh, true, append);
    requestSizes(fresh);
    reportHubStatus();
    updateLoadMore();
    updateActionButtons();
}

// Ask the backend for the download size of newly shown results. Sizes need one Hub
// request per model, so they arrive after the cards do and are purely additive: a
// failure leaves the cards as they are.
void ModelsPanel::requestSizes(const QJsonArray &results) {
    if (results.isEmpty() || !m_client || !m_client->isReady()) return;
    QUrlQuery q;
    for (const auto &value : results)
        q.addQueryItem("repo", value.toObject().value("repo_id").toString());
    m_client->get("/models/sizes?" + q.toString(QUrl::FullyEncoded),
                  [this](int status, const QJsonDocument &doc) {
        if (status != 200 || !doc.isObject() || isLocalView()) return;
        const QJsonObject sizes = doc.object();
        for (auto it = sizes.begin(); it != sizes.end(); ++it)
            if (ModelCard *card = cardById(it.key()))   // gone if the results were replaced meanwhile
                card->setSize(it.value().isNull() ? 0 : static_cast<qint64>(it.value().toDouble()));
    });
}

void ModelsPanel::updateLoadMore() {
    m_loadMoreButton->setVisible(!isLocalView() && m_hasMoreResults);
}

void ModelsPanel::reportHubStatus() {
    if (m_cards.isEmpty()) {
        setStatus("No results. Try a different search term.");
        return;
    }
    int downloaded = 0;
    for (const ModelCard *card : m_cards)
        if (card->isDownloaded()) ++downloaded;
    QString text = QString("%1 result(s)").arg(m_cards.size());
    if (downloaded > 0) text += QString(" · %1 already downloaded").arg(downloaded);
    if (m_hasMoreResults) text += " · more available";
    setStatus(text + ". Select one and click Download.");
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
    m_downloadButton->setEnabled(hasSelection && !isLocalView() && m_downloadingId.isEmpty()
                                 && !m_selectedCard->isDownloaded());
    const bool canDelete = m_downloadingId.isEmpty() && !m_deleting;
    m_deleteButton->setEnabled(hasSelection && isLocalView() && canDelete);
    for (ModelCard *card : m_cards) card->setDeleteEnabled(canDelete);
    // Cancel stays reachable from any view for as long as a download is running.
    m_cancelButton->setVisible(!m_downloadingId.isEmpty());
    m_cancelButton->setEnabled(!m_cancelling);
    m_cancelButton->setText(m_cancelling ? "Cancelling…" : "Cancel download");
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

// Forget the running download and put every control back. Safe to call twice.
void ModelsPanel::resetDownload() {
    m_pullTimer->stop();
    m_pullJobId.clear();
    if (auto *card = cardById(m_downloadingId)) card->setProgress(-1);
    m_downloadingId.clear();
    m_cancelPending = false;
    m_cancelling = false;
    m_cancelError.clear();
    updateActionButtons();
}

// Text for the status line and card while a download runs, e.g.
// "Downloading 412.0 MB of 1.8 GB (22%)".
void ModelsPanel::showDownloadProgress(const QJsonObject &job) {
    const double progress = job.value("progress").toDouble();
    const qint64 done = static_cast<qint64>(job.value("bytes_done").toDouble());
    const qint64 total = static_cast<qint64>(job.value("bytes_total").toDouble());
    if (ModelCard *card = cardById(m_downloadingId)) card->setProgress(progress, done, total);
    if (m_cancelling) return;   // keep "Cancelling…" on screen until the server confirms
    const QString text = total > 0
        ? QString("Downloading %1 of %2 (%3%)")
              .arg(ModelCard::formatBytes(done), ModelCard::formatBytes(total))
              .arg(static_cast<int>(progress))
        : QString("Downloading: %1%  (%2)").arg(static_cast<int>(progress))
              .arg(job.value("message").toString());
    if (m_cancelError.isEmpty()) setStatus(text);
    else setStatus(m_cancelError + "  " + text, "error");
}

void ModelsPanel::onDownload() {
    if (!m_downloadingId.isEmpty()) { setStatus("A download is already in progress."); return; }
    const QString ref = selectedId();
    if (ref.isEmpty() || !m_client || !m_client->isReady()) return;
    if (m_selectedCard && m_selectedCard->isDownloaded()) {
        setStatus("Already downloaded. Use Load model to open it.");
        return;
    }

    setStatus(QString("Starting download of \"%1\"...").arg(ref));
    m_downloadingId = ref;
    m_cancelPending = false;
    m_cancelling = false;
    if (ModelCard *card = cardById(ref))
        card->setProgress(0);
    updateActionButtons();

    QJsonObject body;
    body["ref"] = ref;
    body["source"] = "huggingface";
    m_client->post("/models/pull", body, [this](int status, const QJsonDocument &doc) {
        if (status != 202) {
            const QString detail = doc.object().value("detail").toString();
            resetDownload();
            setStatus(detail.isEmpty() ? QString("Could not start download (HTTP %1).").arg(status)
                                       : QString("Could not start download: %1").arg(detail), "error");
            return;
        }
        startPullPolling(doc.object().value("id").toString());
        // Cancel was pressed while the request was in flight: deliver it now.
        if (m_cancelPending) { m_cancelPending = false; sendCancel(); }
    });
}

void ModelsPanel::startPullPolling(const QString &jobId) {
    m_pullJobId = jobId;
    onPullPoll();
    m_pullTimer->start();
}

void ModelsPanel::onCancelDownload() {
    if (m_downloadingId.isEmpty() || m_cancelling) return;
    m_cancelling = true;
    m_cancelError.clear();
    if (ModelCard *card = cardById(m_downloadingId)) card->setCancelling(true);
    setStatus("Cancelling download…");
    updateActionButtons();
    if (m_pullJobId.isEmpty()) {          // the start request has not answered yet
        m_cancelPending = true;
        return;
    }
    sendCancel();
}

void ModelsPanel::sendCancel() {
    if (m_pullJobId.isEmpty() || !m_client || !m_client->isReady()) return;
    m_client->post("/models/pull/" + m_pullJobId + "/cancel", QJsonObject(),
                   [this](int status, const QJsonDocument &doc) {
        if (status == 200 || status == 202) return;   // polling reports when it has actually stopped
        if (m_downloadingId.isEmpty()) return;        // the download ended while we were asking
        // The download is still running: let the user try again.
        const QString detail = doc.object().value("detail").toString();
        m_cancelling = false;
        m_cancelError = detail.isEmpty() ? QString("Could not cancel the download (HTTP %1).").arg(status)
                                         : QString("Could not cancel the download: %1").arg(detail);
        if (ModelCard *card = cardById(m_downloadingId)) card->setCancelling(false);
        updateActionButtons();
        setStatus(m_cancelError, "error");
    });
}

void ModelsPanel::onPullPoll() {
    if (m_pullJobId.isEmpty() || !m_client || !m_client->isReady()) {
        resetDownload();
        return;
    }
    m_client->get("/models/pull/" + m_pullJobId,
                  [this](int status, const QJsonDocument &doc) {
        if (m_pullJobId.isEmpty()) return;   // finished or cancelled while this reply was in flight
        if (status != 200) {
            resetDownload();
            setStatus(QString("Lost track of download job (HTTP %1).").arg(status), "error");
            return;
        }
        const QJsonObject job = doc.object();
        const QString jobStatus = job.value("status").toString();

        if (jobStatus == "done") {
            resetDownload();
            setStatus("Download complete. Refreshing local models...", "ok");
            emit downloadFinished(true);
            switchToLocalView();
            refresh();
        } else if (jobStatus == "error") {
            resetDownload();
            setStatus("Download failed: " + job.value("error").toString(), "error");
            emit downloadFinished(false);
        } else if (jobStatus == "cancelled") {
            resetDownload();
            setStatus("Download cancelled. Partial files were removed.");
        } else {
            showDownloadProgress(job);
        }
    });
}

void ModelsPanel::onDeleteModel() {
    if (m_deleting || !m_selectedCard || !isLocalView() || !m_downloadingId.isEmpty()) return;
    if (!m_client || !m_client->isReady()) return;
    const QJsonObject model = m_selectedCard->data();
    const QString id = model.value("id").toString();
    const QString ref = model.value("ref").toString();
    // Adapters are trained against a base model; tell the user how many this would orphan.
    m_client->get("/adapters", [this, model, id, ref](int status, const QJsonDocument &doc) {
        int dependents = 0;
        if (status == 200 && doc.isArray()) {
            for (const auto &entry : doc.array()) {
                const QString base = entry.toObject().value("base_model").toString();
                if (!base.isEmpty() && (base == id || base == ref)) ++dependents;
            }
        }
        confirmAndDelete(model, dependents);   // a failed lookup just means a plainer message
    });
}

void ModelsPanel::confirmAndDelete(const QJsonObject &model, int dependentAdapters) {
    if (m_deleting || !m_client || !m_client->isReady()) return;
    const QString id = model.value("id").toString();
    const QString source = model.value("source").toString();
    const QString size = ModelCard::formatSizeMb(model.value("size_mb").toDouble());

    QString message = QString("Delete “%1” from your library?\n\nThis removes its files from this computer")
                          .arg(id);
    message += size == "-" ? "." : QString(" and frees about %1.").arg(size);
    if (source == "huggingface")
        message += "\n\nIt is stored in the shared Hugging Face cache, so other tools that use that cache will lose it too.";
    else if (source == "ollama")
        message += "\n\nIt is also removed from Ollama.";
    if (dependentAdapters > 0)
        message += QString("\n\n%1 adapter%2 trained on it will stop working until you download it again.")
                       .arg(dependentAdapters).arg(dependentAdapters == 1 ? "" : "s");
    message += "\n\nYou would need to download it again to use it.";
    if (!Ui::confirmRemoval(this, "Delete model?", message)) return;

    m_deleting = true;
    updateActionButtons();
    setStatus(QString("Deleting “%1”…").arg(id));
    m_client->remove("/models/" + id, [this, id, size](int status, const QJsonDocument &doc) {
        m_deleting = false;
        if (status == 204) {
            m_afterRefreshNote = size == "-" ? QString("Deleted “%1”.").arg(id)
                                             : QString("Deleted “%1”. Freed about %2.").arg(id, size);
            setStatus(m_afterRefreshNote);
            refresh();
        } else if (status == 404) {
            m_afterRefreshNote = "That model was already removed.";
            setStatus(m_afterRefreshNote);
            refresh();
        } else {
            const QString detail = doc.object().value("detail").toString();
            setStatus(detail.isEmpty() ? QString("Could not delete (HTTP %1).").arg(status)
                                       : QString("Could not delete: %1").arg(detail), "error");
        }
        updateActionButtons();
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
