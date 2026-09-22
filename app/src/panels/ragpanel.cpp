#include <QSplitter>
#include "panels/ragpanel.h"

#include "backend_client.h"
#include "widgets/surface.h"
#include "panels/ragdoccard.h"

#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

RagPanel::RagPanel(QWidget *parent)
    : BasePanel("Knowledge & retrieval", parent)
    , m_client(nullptr)
    , m_addButton(nullptr)
    , m_removeButton(nullptr)
    , m_clearButton(nullptr)
    , m_docScroll(nullptr)
    , m_docHost(nullptr)
    , m_docLayout(nullptr)
    , m_emptyState(nullptr)
    , m_chunkSizeSpin(nullptr)
    , m_chunkOverlapSpin(nullptr)
    , m_topKSpin(nullptr)
    , m_queryEdit(nullptr)
    , m_resultLabel(nullptr)
    , m_statusLabel(nullptr)
    , m_loadingConfig(false)
{
    setHeaderSubtitle(
        "Attach a local knowledge base: ingest documents, embed them, and inject "
        "retrieved context during chat.");
    setupUi();
}

void RagPanel::setupUi() {
    auto *layout = contentLayout();

    auto *btnRow = new QHBoxLayout();
    m_addButton = new QPushButton("Add Documents...", this);
    m_addButton->setObjectName("primaryBtn");
    m_removeButton = new QPushButton("Remove Selected", this);
    m_removeButton->setObjectName("secondaryBtn");
    m_removeButton->setEnabled(false);
    m_clearButton = new QPushButton("Clear All", this);
    m_clearButton->setObjectName("destructiveBtn");
    btnRow->addWidget(m_addButton);
    btnRow->addWidget(m_removeButton);
    btnRow->addWidget(m_clearButton);
    btnRow->addStretch();
    layout->addLayout(btnRow);

    auto *columns = new QSplitter(Qt::Horizontal,this);
    columns->setChildrenCollapsible(false);
    columns->setHandleWidth(4);
    layout->addWidget(columns,1);
    auto *inspector = new QWidget(this);
    auto *bench = new QVBoxLayout(inspector);
    bench->setContentsMargins(0,0,0,0);
    bench->setSpacing(8);
    auto *documents = Surface::card(this,"Source collection");
    columns->addWidget(documents->parentWidget());
    columns->addWidget(inspector);
    columns->setSizes({650,450});
    // Document list.
    m_docScroll = new QScrollArea(this);
    m_docScroll->setWidgetResizable(true);
    m_docScroll->setFrameShape(QFrame::NoFrame);
    m_docScroll->setObjectName("cardScroll");
    m_docScroll->viewport()->setAutoFillBackground(false);
    m_docHost = new QWidget(this);
    m_docLayout = new QVBoxLayout(m_docHost);
    m_docLayout->setContentsMargins(2, 2, 2, 2);
    m_docLayout->setSpacing(8);
    m_docScroll->setWidget(m_docHost);
    m_docScroll->setMinimumHeight(200);
    documents->addWidget(m_docScroll, 1);

    // Empty state (shown instead of the scroll area when no docs are added).
    m_emptyState = new QWidget(this);
    auto *emptyLayout = new QVBoxLayout(m_emptyState);
    emptyLayout->setContentsMargins(12, 12, 12, 12);
    emptyLayout->setAlignment(Qt::AlignCenter);
    auto *hint = new QLabel("No documents ingested yet.\nAdd PDFs, text, markdown or HTML above.", m_emptyState);
    hint->setObjectName("mutedLabel");
    hint->setAlignment(Qt::AlignCenter);
    hint->setWordWrap(true);
    emptyLayout->addWidget(hint);
    m_emptyState->setLayout(emptyLayout);
    documents->addWidget(m_emptyState, 1);

    // Retrieval config as a glass card (design doc §4/§8).
    auto *configCard = new QFrame(this);
    configCard->setObjectName("ragConfigCard");
    auto *configLayout = new QVBoxLayout(configCard);
    configLayout->setContentsMargins(16, 14, 16, 14);
    configLayout->setSpacing(10);
    auto *configTitle = new QLabel("Retrieval Configuration", this);
    configTitle->setObjectName("cardTitle");
    configLayout->addWidget(configTitle);
    auto *form = new QFormLayout();
    form->setSpacing(8);
    form->setRowWrapPolicy(QFormLayout::WrapAllRows);

    m_chunkSizeSpin = new QSpinBox(this);
    m_chunkSizeSpin->setRange(64, 4096);
    m_chunkSizeSpin->setSingleStep(64);
    m_chunkSizeSpin->setValue(512);
    form->addRow("Chunk size (tokens):", m_chunkSizeSpin);

    m_chunkOverlapSpin = new QSpinBox(this);
    m_chunkOverlapSpin->setRange(0, 1024);
    m_chunkOverlapSpin->setSingleStep(16);
    m_chunkOverlapSpin->setValue(64);
    form->addRow("Chunk overlap:", m_chunkOverlapSpin);

    m_topKSpin = new QSpinBox(this);
    m_topKSpin->setRange(1, 20);
    m_topKSpin->setValue(4);
    form->addRow("Top-K retrieved chunks:", m_topKSpin);

    configLayout->addLayout(form);

    configLayout->addStretch();
    bench->addWidget(configCard);
    auto *retrieval = Surface::card(this,"Query laboratory");
    bench->addWidget(retrieval->parentWidget(),1);
    // Test-retrieval row.
    auto *queryRow = new QHBoxLayout();
    m_queryEdit = new QLineEdit(this);
    m_queryEdit->setPlaceholderText("Test a query against the ingested documents...");
    m_queryEdit->setClearButtonEnabled(true);
    auto *searchButton = new QPushButton("Search", this);
    searchButton->setObjectName("secondaryBtn");
    queryRow->addWidget(m_queryEdit, 1);
    queryRow->addWidget(searchButton);
    retrieval->addLayout(queryRow);

    m_resultLabel = new QLabel(this);
    m_resultLabel->setObjectName("monoLabel");
    m_resultLabel->setWordWrap(true);
    m_resultLabel->setText(" ");
    retrieval->addWidget(m_resultLabel);
    retrieval->addStretch();



    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName("statusLabel");
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);
    setStatus("RAG engine starting...");

    connect(m_addButton, &QPushButton::clicked, this, &RagPanel::onAddDocuments);
    connect(m_removeButton, &QPushButton::clicked, this, &RagPanel::onRemoveDocument);
    connect(m_clearButton, &QPushButton::clicked, this, &RagPanel::onClearAll);
    connect(searchButton, &QPushButton::clicked, this, &RagPanel::onTestQuery);
    connect(m_queryEdit, &QLineEdit::returnPressed, this, &RagPanel::onTestQuery);
    connect(m_chunkSizeSpin, qOverload<int>(&QSpinBox::valueChanged),
            this, &RagPanel::onConfigChanged);
    connect(m_chunkOverlapSpin, qOverload<int>(&QSpinBox::valueChanged),
            this, &RagPanel::onConfigChanged);
    connect(m_topKSpin, qOverload<int>(&QSpinBox::valueChanged),
            this, &RagPanel::onConfigChanged);

    updateEmptyState();
}

void RagPanel::setBackendClient(BackendClient *client) {
    m_client = client;
    if (client && client->isReady()) {
        refresh();
    }
}

void RagPanel::refresh() {
    if (!m_client || !m_client->isReady()) {
        setStatus("Backend not connected.");
        return;
    }

    m_client->get("/rag/status", [this](int status, const QJsonDocument &doc) {
        if (status != 200) {
            setStatus(QString("RAG status unavailable (HTTP %1).").arg(status), "error");
            return;
        }
        const QJsonObject obj = doc.object();
        const QString model = obj["model"].toString();
        const int docs = obj["documents"].toInt();
        const int chunks = obj["chunks"].toInt();
        const bool engineReady = obj["engine_ready"].toBool();
        setStatus(engineReady
                      ? QString("%1 document(s) · %2 chunks · model %3")
                            .arg(docs).arg(chunks).arg(model)
                      : "Embeddings unavailable — RAG needs sentence-transformers "
                        "(backend[ml]).");
        m_resultLabel->setText(" ");
    });

    m_client->get("/rag/config", [this](int status, const QJsonDocument &doc) {
        if (status != 200) return;
        const QJsonObject cfg = doc.object();
        m_loadingConfig = true;
        if (cfg.contains("chunk_size")) m_chunkSizeSpin->setValue(cfg["chunk_size"].toInt());
        if (cfg.contains("chunk_overlap")) m_chunkOverlapSpin->setValue(cfg["chunk_overlap"].toInt());
        if (cfg.contains("top_k")) m_topKSpin->setValue(cfg["top_k"].toInt());
        m_loadingConfig = false;
    });

    m_client->get("/rag/documents", [this](int status, const QJsonDocument &doc) {
        if (status != 200) {
            setStatus(QString("Failed to load documents (HTTP %1).").arg(status), "error");
            return;
        }
        clearCards();
        const QJsonArray arr = doc.array();
        for (const auto &value : arr) {
            if (!value.isObject()) continue;
            auto *card = new RagDocCard(value.toObject(), m_docHost);
            connect(card, &RagDocCard::cardClicked, this, &RagPanel::onCardClicked);
            m_docLayout->addWidget(card);
            m_cards.append(card);
        }
        m_docLayout->invalidate();
        m_docHost->updateGeometry();
        updateEmptyState();
    });
}

void RagPanel::onAddDocuments() {
    if (!m_client || !m_client->isReady()) {
        setStatus("Backend not connected.", "error");
        return;
    }
    const QStringList files = QFileDialog::getOpenFileNames(
        this, "Select documents",
        QString(),
        "Documents (*.pdf *.txt *.md *.html);;All Files (*)");
    if (files.isEmpty()) return;

    setStatus(QString("Ingesting %1 document(s) — first use downloads the "
                      "embedding model...").arg(files.size()));
    int *pending = new int(files.size());
    for (const QString &f : files) {
        QJsonObject body;
        body["path"] = f;
        m_client->post("/rag/documents", body,
            [this, pending, filesSize = files.size()](int status, const QJsonDocument &doc) {
                --(*pending);
                if (status == 201) {
                    setStatus(QString("%1/%2 ingested — embedding chunks...")
                                  .arg(filesSize - *pending).arg(filesSize));
                } else {
                    const QString err = doc.object()["detail"].toString();
                    setStatus(QString("Ingest failed: %1").arg(
                                  err.isEmpty() ? QString("HTTP %1").arg(status) : err),
                              "error");
                }
                if (*pending <= 0) {
                    delete pending;
                    refresh();
                }
            });
    }
}

void RagPanel::onCardClicked(RagDocCard *card, bool ctrl) {
    if (!card) return;
    if (!ctrl) {
        for (RagDocCard *c : m_cards) {
            if (c != card && c->isCardSelected())
                c->setSelected(false);
        }
        card->setSelected(true);
    } else {
        card->setSelected(!card->isCardSelected());
    }
    m_removeButton->setEnabled(hasSelection());
}

bool RagPanel::hasSelection() const {
    for (const RagDocCard *c : m_cards) {
        if (c->isCardSelected())
            return true;
    }
    return false;
}

QString RagPanel::selectedIds() const {
    QStringList ids;
    for (const RagDocCard *c : m_cards) {
        if (c->isCardSelected())
            ids.append(c->id());
    }
    return ids.join(',');
}

void RagPanel::onRemoveDocument() {
    if (!m_client || !m_client->isReady()) return;
    const QStringList ids = selectedIds().split(',', Qt::SkipEmptyParts);
    if (ids.isEmpty()) {
        setStatus("No documents selected.");
        return;
    }
    int *pending = new int(ids.size());
    for (const QString &id : ids) {
        m_client->remove("/rag/documents/" + id,
            [this, pending](int status, const QJsonDocument &) {
                --(*pending);
                if (status != 204) setStatus("Failed to remove a document.", "error");
                if (*pending <= 0) {
                    delete pending;
                    m_removeButton->setEnabled(false);
                    refresh();
                }
            });
    }
}

void RagPanel::onClearAll() {
    if (!m_client || !m_client->isReady()) return;
    m_client->post("/rag/clear", QJsonObject(), [this](int status, const QJsonDocument &) {
        if (status != 204) {
            setStatus("Failed to clear the store.", "error");
            return;
        }
        clearCards();
        setStatus("Cleared all documents.");
    });
}

void RagPanel::onTestQuery() {
    if (!m_client || !m_client->isReady()) {
        setStatus("Backend not connected.", "error");
        return;
    }
    const QString query = m_queryEdit->text().trimmed();
    if (query.isEmpty()) return;
    QJsonObject body;
    body["query"] = query;
    body["top_k"] = m_topKSpin->value();
    setStatus("Embedding query...");
    m_client->post("/rag/query", body, [this, query](int status, const QJsonDocument &doc) {
        if (status != 200) {
            const QString err = doc.object()["detail"].toString();
            setStatus(QString("Query failed: %1").arg(
                          err.isEmpty() ? QString("HTTP %1").arg(status) : err), "error");
            return;
        }
        const QJsonArray results = doc.object()["results"].toArray();
        if (results.isEmpty()) {
            m_resultLabel->setText("No matching chunks above the similarity threshold.");
            setStatus("Query returned nothing.");
            return;
        }
        QStringList lines;
        const int shown = qMin(3, results.size());
        for (int i = 0; i < shown; ++i) {
            const QJsonObject r = results[i].toObject();
            QString text = r["text"].toString();
            if (text.size() > 140) text = text.left(137) + "...";
            text.replace('\n', ' ');
            lines.append(QStringLiteral("[%1 · %2#%3] %4")
                             .arg(r["score"].toDouble(), 0, 'f', 2)
                             .arg(r["document_name"].toString())
                             .arg(r["index"].toInt() + 1)
                             .arg(text));
        }
        m_resultLabel->setText(lines.join('\n'));
        setStatus(QString("Retrieved %1 chunk(s) for “%2”.")
                      .arg(results.size()).arg(query), "ok");
    });
}

void RagPanel::onConfigChanged() {
    if (m_loadingConfig) return;
    pushConfig();
}

void RagPanel::pushConfig() {
    if (!m_client || !m_client->isReady()) return;
    QJsonObject body;
    body["chunk_size"] = m_chunkSizeSpin->value();
    body["chunk_overlap"] = m_chunkOverlapSpin->value();
    body["top_k"] = m_topKSpin->value();
    m_client->post("/rag/config", body, [](int, const QJsonDocument &) {});
}

void RagPanel::clearCards() {
    while (!m_cards.isEmpty()) {
        RagDocCard *c = m_cards.takeFirst();
        m_docLayout->removeWidget(c);
        c->hide();
        c->setParent(nullptr);
        c->deleteLater();
    }
    m_docLayout->invalidate();
    m_docHost->updateGeometry();
    m_removeButton->setEnabled(false);
    updateEmptyState();
}

void RagPanel::updateEmptyState() {
    const bool hasDocs = !m_cards.isEmpty();
    m_docScroll->setVisible(hasDocs);
    m_emptyState->setVisible(!hasDocs);
}

void RagPanel::setStatus(const QString &text, const QString &tone) {
    m_statusLabel->setText(text);
    applyStatusTone(m_statusLabel, tone);
}

QString RagPanel::settingsJson() const {
    QJsonObject obj;
    obj["chunkSize"] = m_chunkSizeSpin->value();
    obj["chunkOverlap"] = m_chunkOverlapSpin->value();
    obj["topK"] = m_topKSpin->value();
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void RagPanel::applySettingsJson(const QString &json) {
    QJsonParseError err;
    auto doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return;
    m_loadingConfig = true;
    auto o = doc.object();
    if (o.contains("chunkSize")) m_chunkSizeSpin->setValue(o["chunkSize"].toInt());
    if (o.contains("chunkOverlap")) m_chunkOverlapSpin->setValue(o["chunkOverlap"].toInt());
    if (o.contains("topK")) m_topKSpin->setValue(o["topK"].toInt());
    m_loadingConfig = false;
}