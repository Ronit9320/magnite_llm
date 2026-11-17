#include "widgets/spinbox.h"
#include "widgets/confirmation.h"
#include <QTimer>
#include <memory>

#include "panels/ragpanel.h"

#include "backend_client.h"
#include "widgets/surface.h"
#include "panels/ragdoccard.h"

#include <QFileDialog>
#include <QCheckBox>
#include <QSignalBlocker>
#include <QFormLayout>
#include <QFrame>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include "widgets/button.h"
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

RagPanel::RagPanel(QWidget *parent)
    : BasePanel("Knowledge & retrieval", parent, true)
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
    embedInWorkspace();
    setupUi();
}

void RagPanel::setupUi() {
    auto *layout = contentLayout();

    auto *btnRow = new QHBoxLayout();
    m_addButton = new Button("Add files…", this);
    m_addButton->setObjectName("primaryBtn");
    m_removeButton = new Button("Remove", this);
    m_removeButton->setObjectName("secondaryBtn");
    m_removeButton->setEnabled(false);
    m_clearButton = new Button("Clear all", this);
    m_clearButton->setObjectName("destructiveBtn");
    btnRow->addWidget(m_addButton);
    btnRow->addWidget(m_removeButton);
    btnRow->addWidget(m_clearButton);
    auto *documents = Surface::card(this, "Sources");
    documents->addLayout(btnRow);
    documents->parentWidget()->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    layout->addWidget(documents->parentWidget());

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
    m_docLayout->setAlignment(Qt::AlignTop);
    m_docScroll->setWidget(m_docHost);
    m_docScroll->setMinimumHeight(90);
    m_docScroll->setMaximumHeight(210);
    documents->addWidget(m_docScroll, 1);

    // Empty state (shown instead of the scroll area when no docs are added).
    m_emptyState = new QWidget(this);
    auto *emptyLayout = new QVBoxLayout(m_emptyState);
    emptyLayout->setContentsMargins(12, 12, 12, 12);
    emptyLayout->setAlignment(Qt::AlignCenter);
    auto *hint = new QLabel("No documents yet.", m_emptyState);
    hint->setObjectName("mutedLabel");
    hint->setAlignment(Qt::AlignCenter);
    hint->setWordWrap(true);
    emptyLayout->addWidget(hint);
    m_emptyState->setLayout(emptyLayout);
    documents->addWidget(m_emptyState);

    // The embedding model and chunking the backend is really using (GET /rag/status).
    auto *modelCard = Surface::card(this, "Embedding model");
    modelCard->parentWidget()->setObjectName("ragModelCard");
    modelCard->parentWidget()->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Maximum);
    m_modelInfo = new QLabel("Reading the embedding model…", this);
    m_modelInfo->setObjectName("ragModelInfo");
    m_modelInfo->setTextFormat(Qt::PlainText);
    m_modelInfo->setWordWrap(true);
    modelCard->addWidget(m_modelInfo);
    m_chunkingInfo = new QLabel(this);
    m_chunkingInfo->setObjectName("ragChunkingInfo");
    m_chunkingInfo->setTextFormat(Qt::PlainText);
    m_chunkingInfo->setWordWrap(true);
    modelCard->addWidget(m_chunkingInfo);
    m_clampHint = new QLabel(this);
    m_clampHint->setObjectName("ragClampHint");
    m_clampHint->setTextFormat(Qt::PlainText);
    m_clampHint->setWordWrap(true);
    m_clampHint->hide();
    modelCard->addWidget(m_clampHint);
    layout->addWidget(modelCard->parentWidget());

    // Retrieval config as a surface card (design doc §4/§8).
    auto *configCard = new QFrame(this);
    configCard->setObjectName("ragConfigCard");
    auto *configLayout = new QVBoxLayout(configCard);
    configLayout->setContentsMargins(16, 14, 16, 14);
    configLayout->setSpacing(10);
    auto *form = new QFormLayout();
    form->setSpacing(8);
    form->setRowWrapPolicy(QFormLayout::WrapAllRows);

    // No values until GET /rag/status says what the configured model really uses.
    m_chunkSizeSpin = new SpinBox(this);
    m_chunkSizeSpin->setObjectName("ragChunkSize");
    m_chunkSizeSpin->setSingleStep(16);
    form->addRow("Chunk size (words):", m_chunkSizeSpin);

    m_chunkOverlapSpin = new SpinBox(this);
    m_chunkOverlapSpin->setObjectName("ragChunkOverlap");
    m_chunkOverlapSpin->setSingleStep(8);
    form->addRow("Chunk overlap (words):", m_chunkOverlapSpin);

    m_topKSpin = new SpinBox(this);
    m_topKSpin->setObjectName("ragTopK");
    m_topKSpin->setRange(1, 20);                      // the backend's own bounds for top_k
    form->addRow("Top-K retrieved chunks:", m_topKSpin);
    for (QSpinBox *spin : {m_chunkSizeSpin, m_chunkOverlapSpin, m_topKSpin}) {
        spin->setSpecialValueText(QStringLiteral("—"));
        spin->setMinimum(0);
        spin->setValue(0);
        spin->setEnabled(false);
    }

    configLayout->addLayout(form);

    auto addDisclosure = [this, layout](const QString &title, QWidget *section) {
        auto *toggle = new Button(title, this);
        toggle->setCheckable(true);
        layout->addWidget(toggle);
        layout->addWidget(section);
        section->hide();
        connect(toggle, &QPushButton::toggled, section, &QWidget::setVisible);
    };
    addDisclosure("Retrieval settings", configCard);
    auto *retrieval = Surface::card(this);
    addDisclosure("Test retrieval", retrieval->parentWidget());
    // Test-retrieval row.
    auto *queryRow = new QHBoxLayout();
    m_queryEdit = new QLineEdit(this);
    m_queryEdit->setPlaceholderText("Test a query against the ingested documents...");
    m_queryEdit->setClearButtonEnabled(true);
    m_queryEdit->setObjectName("documentQuery");
    m_queryEdit->setAccessibleName("Search document contents");
    auto *searchButton = m_queryButton = new Button("Search", this);
    searchButton->setObjectName("secondaryBtn");
    queryRow->addWidget(m_queryEdit, 1);
    queryRow->addWidget(searchButton);
    searchButton->setEnabled(false);
    connect(m_queryEdit,&QLineEdit::textChanged,this,[this]() {
        m_queryButton->setEnabled(!m_queryBusy && !m_mutating && !m_queryEdit->text().trimmed().isEmpty());
    });
    retrieval->addLayout(queryRow);

    m_resultLabel = new QLabel(this);
    m_resultLabel->setObjectName("monoLabel");
    m_resultLabel->setWordWrap(true);
    m_resultLabel->setText("Search for a topic or ask a question to preview matching passages.");
    m_resultLabel->setTextFormat(Qt::PlainText);
    retrieval->addWidget(m_resultLabel);
    retrieval->addStretch();



    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName("statusLabel");
    m_statusLabel->setTextFormat(Qt::PlainText);
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);
    layout->addStretch();
    setStatus("Add PDF, text, Markdown or HTML files to get started.");
    m_configTimer = new QTimer(this);
    m_configTimer->setSingleShot(true);
    m_configTimer->setInterval(350);
    connect(m_configTimer,&QTimer::timeout,this,&RagPanel::pushConfig);

    connect(m_addButton, &QPushButton::clicked, this, &RagPanel::onAddDocuments);
    connect(m_removeButton, &QPushButton::clicked, this, &RagPanel::onRemoveDocument);
    connect(m_clearButton, &QPushButton::clicked, this, &RagPanel::onClearAll);
    connect(searchButton, &QPushButton::clicked, this, &RagPanel::onTestQuery);
    connect(m_queryEdit, &QLineEdit::returnPressed, this, &RagPanel::onTestQuery);
    connect(m_chunkSizeSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this] {
        if (m_loadingConfig) return;
        m_dirtyChunking = true;
        onConfigChanged();
    });
    connect(m_chunkOverlapSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this] {
        if (m_loadingConfig) return;
        m_dirtyChunking = true;
        onConfigChanged();
    });
    connect(m_topKSpin, qOverload<int>(&QSpinBox::valueChanged), this, [this] {
        if (m_loadingConfig) return;
        m_dirtyTopK = true;
        onConfigChanged();
    });

    updateEmptyState();
}

void RagPanel::setBackendClient(BackendClient *client) {
    m_client = client;
    if (client && client->isReady()) {
        refresh();
    }
}

void RagPanel::refresh(bool preserveStatus) {
    if (!m_client || !m_client->isReady()) {
        setStatus("Backend not connected.");
        return;
    }

    const quint64 revision = ++m_refreshRevision;
    m_client->get("/rag/embedders", [this, revision](int status, const QJsonDocument &doc) {
        if (revision != m_refreshRevision || status != 200) return;
        m_presets = RagParams::parseEmbedders(doc.array());
        applyStatus();
    });
    m_client->get("/rag/status", [this, revision, preserveStatus](int status, const QJsonDocument &doc) {
        if (revision != m_refreshRevision) return;
        if (status != 200) {
            const QString detail = doc.object()["detail"].toString();
            setStatus(detail.isEmpty() ? QString("Knowledge status unavailable (HTTP %1).").arg(status) : detail, "error");
            return;
        }
        const QJsonObject obj = doc.object();
        m_active = RagParams::parseStatus(obj);
        if (!m_active) {
            setStatus("The backend did not report its embedding parameters; update Magnite.", "error");
            return;
        }
        applyStatus();
        if (!preserveStatus) setStatus(m_active->engineReady
                      ? QString("%1 document(s) · %2 chunks").arg(m_active->documents).arg(m_active->chunks)
                      : "Embeddings unavailable — RAG needs sentence-transformers (backend[ml]).");
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
            connect(card->sourceToggle(), &QCheckBox::toggled, this, [this] {
                QJsonArray selected;
                for (const auto *source : m_cards) {
                    if (source->sourceToggle()->isChecked()) selected.append(source->id());
                }
                setDocumentScope(selected);
            });
        }
        setDocumentScope(m_documentScope);
        m_docLayout->invalidate();
        m_docHost->updateGeometry();
        updateEmptyState();
    });
}

void RagPanel::onAddDocuments() {
    if (m_mutating) return;
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
    m_mutating = true;
    ++m_collectionRevision;
    m_queryButton->setEnabled(false);
    m_addButton->setEnabled(false);
    m_removeButton->setEnabled(false);
    m_clearButton->setEnabled(false);
    auto pending = std::make_shared<int>(files.size());
    auto failed = std::make_shared<QStringList>();
    for (const QString &f : files) {
        m_client->post("/rag/documents", QJsonObject{{"path",f}},
            [this,pending,failed,total=files.size()](int status,const QJsonDocument &doc) {
                --*pending;
                if (status != 201) failed->append(doc.object()["detail"].toString(QString("HTTP %1").arg(status)));
                if (*pending > 0) {
                    setStatus(QString("Processing documents · %1 of %2 finished").arg(total-*pending).arg(total));
                    return;
                }
                m_mutating = false;
                m_addButton->setEnabled(true);
                m_resultLabel->setText("Your document collection changed. Search again for updated results.");
                refresh(true);
                setStatus(failed->isEmpty() ? QString("Added %1 document(s). Ready to search.").arg(total)
                    : QString("Added %1 of %2 documents. %3 failed: %4").arg(total-failed->size()).arg(total).arg(failed->size()).arg(failed->first()),
                    failed->isEmpty() ? "ok" : "error");
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
    m_removeButton->setEnabled(hasSelection() && !m_mutating);
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
    if (m_mutating) return;
    if (!m_client || !m_client->isReady()) return;
    const QStringList ids = selectedIds().split(',', Qt::SkipEmptyParts);
    if (ids.isEmpty()) {
        setStatus("No documents selected.");
        return;
    }
    if (!Ui::confirmRemoval(this,"Remove documents?",QString("Remove %1 selected document(s) from Knowledge? Your original files are kept.").arg(ids.size()),"Remove")) return;
    m_mutating = true;
    ++m_collectionRevision;
    m_queryButton->setEnabled(false);
    m_addButton->setEnabled(false);
    m_removeButton->setEnabled(false);
    m_clearButton->setEnabled(false);
    auto pending = std::make_shared<int>(ids.size());
    auto failed = std::make_shared<int>(0);
    for (const QString &id : ids) {
        m_client->remove("/rag/documents/" + id,
            [this,pending,failed](int status,const QJsonDocument &) {
                --*pending;
                if (status != 204) ++*failed;
                if (*pending) return;
                m_mutating = false;
                m_addButton->setEnabled(true);
                m_resultLabel->setText("Your document collection changed. Search again for updated results.");
                refresh(true);
                setStatus(*failed ? QString("Could not remove %1 document(s). Try again.").arg(*failed) : "Selected documents removed.",
                          *failed ? "error" : "ok");
            });
    }
}

void RagPanel::onClearAll() {
    if (m_mutating) return;
    if (!m_client || !m_client->isReady()) return;
    if (!Ui::confirmRemoval(this,"Clear Knowledge?","Remove all indexed documents from Knowledge? Your original files are kept.","Clear Knowledge")) return;
    m_mutating = true;
    ++m_collectionRevision;
    m_queryButton->setEnabled(false);
    m_addButton->setEnabled(false);
    m_removeButton->setEnabled(false);
    m_clearButton->setEnabled(false);
    m_client->post("/rag/clear", QJsonObject(), [this](int status, const QJsonDocument &) {
        m_mutating = false;
        m_addButton->setEnabled(true);
        m_removeButton->setEnabled(hasSelection());
        updateEmptyState();
        if (status != 204) {
            setStatus("Failed to clear the store.", "error");
            return;
        }
        clearCards();
        emit sourceScopeChanged();
        m_resultLabel->setText("Add documents to start searching your knowledge.");
        setStatus("Cleared all documents.");
    });
}

void RagPanel::onTestQuery() {
    if (m_queryBusy || m_mutating) return;
    if (!m_client || !m_client->isReady()) {
        setStatus("Backend not connected.", "error");
        return;
    }
    const QString query = m_queryEdit->text().trimmed();
    if (query.isEmpty()) return;
    QJsonObject body;
    body["query"] = query;
    body["top_k"] = m_topKSpin->value();
    if (m_documentScope.isArray()) body["document_ids"] = m_documentScope;
    m_queryBusy = true;
    m_queryButton->setEnabled(false);
    m_queryButton->setText("Searching…");
    m_resultLabel->setText("Finding relevant passages…");
    setStatus("Searching your documents…");
    const quint64 revision = m_collectionRevision;
    m_client->post("/rag/query", body, [this, query,revision](int status, const QJsonDocument &doc) {
        m_queryBusy = false;
        m_queryButton->setText("Search");
        m_queryButton->setEnabled(!m_mutating && !m_queryEdit->text().trimmed().isEmpty());
        if (revision != m_collectionRevision) return;
        if (status != 200) {
            const QString err = doc.object()["detail"].toString();
            m_resultLabel->setText("Search could not finish. Check the message below and try again.");
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
    m_chunkOverlapSpin->setMaximum(qMax(0, qMin(1024, m_chunkSizeSpin->value() - 1)));
    m_configTimer->start();
}

void RagPanel::pushConfig() {
    if (!m_client || !m_client->isReady()) return;
    // Send only what the user changed: Top-K alone must not rewrite the stored chunking.
    QJsonObject body;
    if (m_dirtyChunking) {
        body["chunk_size"] = m_chunkSizeSpin->value();
        body["chunk_overlap"] = m_chunkOverlapSpin->value();
    }
    if (m_dirtyTopK) body["top_k"] = m_topKSpin->value();
    m_dirtyChunking = m_dirtyTopK = false;
    if (body.isEmpty()) return;
    m_client->post("/rag/config", body, [this](int status, const QJsonDocument &doc) {
        if (status != 200) {
            const QString detail = doc.object()["detail"].toString();
            setStatus("Settings not saved: " + (detail.isEmpty() ? QString("HTTP %1").arg(status) : detail), "error");
        }
        refresh(true);
    });
}

void RagPanel::applyStatus() {
    if (!m_active) return;
    const RagParams::Active &a = *m_active;
    QString device = a.loaded
        ? QString("Loaded on %1%2").arg(a.device.isEmpty() ? QStringLiteral("unknown device") : a.device,
                                         a.dtype.isEmpty() ? QString() : QString(" (%1)").arg(a.dtype))
        : QStringLiteral("Not loaded (loads on first use)");
    const QString dims = a.dim > 0 ? QString("%1 dimensions").arg(a.dim) : QStringLiteral("dimensions known after first use");
    const QString tokens = a.maxTokens > 0 ? QString("%1 tokens").arg(a.maxTokens) : QStringLiteral("token limit known after first use");
    m_modelInfo->setText(QString("%1 · %2\n%3 · %4 · %5").arg(a.label, a.model, dims, tokens, device));
    m_chunkingInfo->setText(QString("Chunks of %1 words with %2 words of overlap").arg(a.chunkSize).arg(a.chunkOverlap));
    m_clampHint->setText(QString("Clamped for this model: its %1-token limit fits about %2 words per chunk.")
                             .arg(a.maxTokens).arg(a.maxChunkSize));
    m_clampHint->setVisible(a.clamped);

    m_loadingConfig = true;
    const int maxSize = a.maxChunkSize > 0 ? a.maxChunkSize : 4096;
    for (QSpinBox *spin : {m_chunkSizeSpin, m_chunkOverlapSpin, m_topKSpin}) {
        spin->setSpecialValueText(QString());
        spin->setEnabled(!m_mutating);
    }
    m_chunkSizeSpin->setRange(qMin(64, maxSize), maxSize);
    m_chunkSizeSpin->setValue(a.chunkSize);
    m_chunkOverlapSpin->setRange(0, qMax(0, qMin(1024, a.chunkSize - 1)));
    m_chunkOverlapSpin->setValue(a.chunkOverlap);
    m_topKSpin->setMinimum(1);
    if (a.topK > 0) m_topKSpin->setValue(a.topK);
    m_loadingConfig = false;
}

void RagPanel::setDocumentScope(const QJsonValue &scope) {
    if (scope != m_documentScope) {
        ++m_collectionRevision;
        m_resultLabel->setText("Search enabled sources to preview matching passages.");
    }
    m_documentScope = scope;
    const auto ids = scope.toArray();
    for (auto *card : m_cards) {
        const QSignalBlocker guard(card->sourceToggle());
        card->sourceToggle()->setChecked(!scope.isArray() || ids.contains(card->id()));
    }
    emit sourceScopeChanged();
}

int RagPanel::enabledSourceCount() const {
    int count = 0;
    for (const auto *card : m_cards) count += card->sourceToggle()->isChecked();
    return count;
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
    m_queryButton->setEnabled(!m_queryBusy && !m_mutating && !m_queryEdit->text().trimmed().isEmpty());
    m_clearButton->setEnabled(hasDocs && !m_mutating);
    m_docScroll->setVisible(hasDocs);
    m_emptyState->setVisible(!hasDocs);
}

void RagPanel::setStatus(const QString &text, const QString &tone) {
    m_statusLabel->setText(text);
    applyStatusTone(m_statusLabel, tone);
}

QString RagPanel::settingsJson() const {
    QJsonObject obj;
    obj["topK"] = m_topKSpin->value();   // chunking always comes from the backend
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

void RagPanel::applySettingsJson(const QString &json) {
    QJsonParseError err;
    auto doc = QJsonDocument::fromJson(json.toUtf8(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        return;
    m_loadingConfig = true;
    auto o = doc.object();
    if (o.contains("topK") && m_topKSpin->isEnabled()) m_topKSpin->setValue(o["topK"].toInt());
    m_loadingConfig = false;
}
