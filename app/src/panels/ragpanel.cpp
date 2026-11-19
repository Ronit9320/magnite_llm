#include "widgets/spinbox.h"
#include "widgets/combobox.h"
#include "widgets/confirmation.h"
#include <QEvent>
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
#include <QLocale>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include "widgets/button.h"
#include "widgets/progressbar.h"
#include <QNetworkReply>
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

    layout->setSpacing(14);
    auto *documents = Surface::card(this);
    documents->parentWidget()->setObjectName("ragDocumentsCard");
    documents->setSpacing(10);
    auto *documentsHead = new QHBoxLayout();
    documentsHead->setSpacing(8);
    auto *documentsTitles = new QVBoxLayout();
    documentsTitles->setSpacing(2);
    auto *documentsTitle = new QLabel("Documents", this);
    documentsTitle->setObjectName("ragSectionTitle");
    documentsTitles->addWidget(documentsTitle);
    m_countLabel = new QLabel("Nothing indexed yet", this);
    m_countLabel->setObjectName("ragCount");
    m_countLabel->setTextFormat(Qt::PlainText);
    m_countLabel->setWordWrap(true);
    m_countLabel->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    documentsTitles->addWidget(m_countLabel);
    documentsHead->addLayout(documentsTitles, 1);
    m_addButton = new Button("Add files…", this);
    m_addButton->setObjectName("primaryBtn");
    m_addButton->setIcon(Theme::coloredIcon("plus", QColor(Theme::accentText()), 14));
    m_addButton->setToolTip("Index PDF, text, Markdown or HTML files. Your files stay where they are.");
    documentsHead->addWidget(m_addButton, 0, Qt::AlignTop);
    documents->addLayout(documentsHead);
    m_removeButton = new Button("Remove", this);
    m_removeButton->setObjectName("ragQuietAction");
    m_removeButton->setFlat(true);
    m_removeButton->setEnabled(false);
    m_removeButton->setToolTip("Remove the selected documents from Knowledge. Your files are kept.");
    m_clearButton = new Button("Clear all", this);
    m_clearButton->setObjectName("ragQuietDanger");
    m_clearButton->setFlat(true);
    m_clearButton->setProperty("danger", true);
    m_clearButton->setToolTip("Remove every document from Knowledge. Your files are kept.");

    // Ingest/reindex progress (GET /rag/activity), shown only while such a request is open.
    m_progressBox = new QWidget(this);
    m_progressBox->setObjectName("ragProgress");
    auto *progressLayout = new QVBoxLayout(m_progressBox);
    progressLayout->setContentsMargins(0, 0, 0, 0);
    progressLayout->setSpacing(6);
    m_progressLabel = new QLabel(m_progressBox);
    m_progressLabel->setObjectName("ragProgressLabel");
    m_progressLabel->setTextFormat(Qt::PlainText);
    m_progressLabel->setWordWrap(true);
    auto *progressRow = new QHBoxLayout();
    m_progressBar = new ProgressBar(m_progressBox);
    m_progressBar->setObjectName("ragProgressBar");
    m_progressBar->setTextVisible(false);
    m_progressBar->setAccessibleName("Indexing progress");
    m_cancelButton = new Button("Cancel", m_progressBox);
    m_cancelButton->setObjectName("secondaryBtn");
    progressRow->addWidget(m_progressBar, 1);
    progressRow->addWidget(m_cancelButton);
    progressLayout->addWidget(m_progressLabel);
    progressLayout->addLayout(progressRow);
    m_progressBox->hide();
    documents->addWidget(m_progressBox);
    m_activityTimer = new QTimer(this);
    m_activityTimer->setInterval(500);
    connect(m_activityTimer, &QTimer::timeout, this, &RagPanel::pollActivity);
    connect(m_cancelButton, &QPushButton::clicked, this, &RagPanel::cancelOperations);
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
    m_emptyState->setObjectName("ragEmptyState");
    auto *emptyLayout = new QVBoxLayout(m_emptyState);
    emptyLayout->setContentsMargins(12, 18, 12, 14);
    emptyLayout->setSpacing(6);
    emptyLayout->setAlignment(Qt::AlignCenter);
    auto *emptyIcon = new QLabel(m_emptyState);
    emptyIcon->setObjectName("ragEmptyIcon");
    emptyIcon->setFixedSize(40, 40);
    emptyIcon->setAlignment(Qt::AlignCenter);
    emptyIcon->setProperty("ragIcon", "database");
    emptyIcon->setPixmap(Theme::coloredIcon("database", QColor(Theme::accentLabel()), 20).pixmap(20, 20));
    emptyLayout->addWidget(emptyIcon, 0, Qt::AlignHCenter);
    auto *emptyTitle = new QLabel("No documents yet", m_emptyState);
    emptyTitle->setObjectName("ragEmptyTitle");
    emptyTitle->setAlignment(Qt::AlignCenter);
    emptyLayout->addWidget(emptyTitle);
    auto *hint = new QLabel("Add PDF, text, Markdown or HTML files. When Knowledge is on, the model reads the "
                            "passages that match each question.", m_emptyState);
    hint->setObjectName("ragHint");
    hint->setAlignment(Qt::AlignCenter);
    hint->setWordWrap(true);
    emptyLayout->addWidget(hint);
    m_emptyState->setLayout(emptyLayout);
    documents->addWidget(m_emptyState);
    m_listActions = new QWidget(this);
    auto *listActions = new QHBoxLayout(m_listActions);
    listActions->setContentsMargins(0, 0, 0, 0);
    listActions->setSpacing(4);
    m_selectionHint = new QLabel("Select documents to remove them", this);
    m_selectionHint->setObjectName("ragHint");
    // Hints wrap rather than widen the drawer (it sizes itself to its content's minimum width).
    m_selectionHint->setWordWrap(true);
    m_selectionHint->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    listActions->addWidget(m_selectionHint, 1);
    listActions->addWidget(m_removeButton);
    listActions->addWidget(m_clearButton);
    documents->addWidget(m_listActions);

    // Try a question: preview the passages a prompt would receive.
    auto *retrieval = Surface::card(this);
    retrieval->parentWidget()->setObjectName("ragTryCard");
    retrieval->setSpacing(8);
    auto *tryTitle = new QLabel("Try a question", this);
    tryTitle->setObjectName("ragSectionTitle");
    retrieval->addWidget(tryTitle);
    layout->addWidget(retrieval->parentWidget());

    // The embedding model and chunking the backend is really using (GET /rag/status).
    auto *modelCard = Surface::card(this, "Embedding model");
    modelCard->parentWidget()->setObjectName("ragModelCard");
    m_modelPicker = new ComboBox(this);
    m_modelPicker->setObjectName("ragModelPicker");
    m_modelPicker->setEnabled(false);
    Surface::field(modelCard, "Model", m_modelPicker);
    m_modelHint = new QLabel(this);
    m_modelHint->setObjectName("ragModelHint");
    m_modelHint->setTextFormat(Qt::PlainText);
    m_modelHint->setWordWrap(true);
    m_modelHint->hide();
    modelCard->addWidget(m_modelHint);
    connect(m_modelPicker, &QComboBox::currentIndexChanged, this, &RagPanel::updateModelHint);
    connect(m_modelPicker, &QComboBox::activated, this, &RagPanel::onModelChosen);
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
    m_searchInfo = new QLabel(this);
    m_searchInfo->setObjectName("ragSearchInfo");
    m_searchInfo->setTextFormat(Qt::PlainText);
    m_searchInfo->setWordWrap(true);
    m_searchInfo->hide();
    modelCard->addWidget(m_searchInfo);
    m_clampHint = new QLabel(this);
    m_clampHint->setObjectName("ragClampHint");
    m_clampHint->setTextFormat(Qt::PlainText);
    m_clampHint->setWordWrap(true);
    m_clampHint->hide();
    modelCard->addWidget(m_clampHint);

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

    // Embedding model and chunking are detail: one disclosure at the bottom holds both.
    auto *settings = new QWidget(this);
    auto *settingsLayout = new QVBoxLayout(settings);
    settingsLayout->setContentsMargins(0, 0, 0, 0);
    settingsLayout->setSpacing(10);
    settingsLayout->addWidget(modelCard->parentWidget());
    settingsLayout->addWidget(configCard);
    auto *settingsToggle = new Button("Retrieval settings", this);
    settingsToggle->setObjectName("ragDisclosure");
    settingsToggle->setCheckable(true);
    settingsToggle->setFlat(true);
    settingsToggle->setProperty("leftAligned", true);
    settingsToggle->setToolTip("Embedding model, chunk size and how many passages are retrieved");
    const auto chevron = [settingsToggle](bool open) {
        settingsToggle->setIcon(Theme::coloredIcon(open ? "chevron-down" : "chevron-right",
                                                   QColor(Theme::textSecondary()), 14));
    };
    chevron(false);
    settingsToggle->setProperty("themeIcon", "chevron-right");
    layout->addWidget(settingsToggle);
    layout->addWidget(settings);
    settings->hide();
    connect(settingsToggle, &QPushButton::toggled, settings, &QWidget::setVisible);
    connect(settingsToggle, &QPushButton::toggled, this, [settingsToggle, chevron](bool open) {
        settingsToggle->setProperty("themeIcon", open ? "chevron-down" : "chevron-right");
        chevron(open);
    });
    // Test-retrieval row.
    auto *queryRow = new QHBoxLayout();
    m_queryEdit = new QLineEdit(this);
    m_queryEdit->setPlaceholderText("Ask something your documents answer…");
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
    m_resultLabel->setObjectName("ragResult");
    m_resultLabel->setWordWrap(true);
    m_resultLabel->setText("See which passages a prompt would receive before you ask the model.");
    m_resultLabel->setTextFormat(Qt::PlainText);
    retrieval->addWidget(m_resultLabel);



    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName("statusLabel");
    m_statusLabel->setTextFormat(Qt::PlainText);
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);
    layout->addStretch();
    setStatus({});
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
        populatePicker();
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
        populatePicker();
        // The count line above the list already says how much is indexed; the status line is for news.
        if (!preserveStatus) setStatus(m_active->engineReady
                      ? QString()
                      : "Embeddings unavailable — RAG needs sentence-transformers (backend[ml]).",
                      m_active->engineReady ? QString() : QString("error"));
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
    ingestFiles(files);
}

void RagPanel::ingestFiles(const QStringList &files) {
    if (m_mutating || files.isEmpty()) return;
    if (!m_client || !m_client->isReady()) {
        setStatus("Backend not connected.", "error");
        return;
    }
    setStatus(QString("Indexing %1 document(s). The first use downloads the embedding model.").arg(files.size()));
    m_cancelRequested = false;
    setMutating(true);
    auto pending = std::make_shared<int>(files.size());
    auto failed = std::make_shared<QStringList>();
    auto cancelled = std::make_shared<int>(0);
    for (const QString &f : files) {
        QNetworkReply *reply = m_client->post("/rag/documents", QJsonObject{{"path", f}},
            [this, pending, failed, cancelled, total = files.size()](int status, const QJsonDocument &doc) {
                --*pending;
                endOperation();
                if (status == 201) {
                } else if (m_cancelRequested && (status == 0 || status == 499)) {
                    ++*cancelled;
                } else {
                    const QString detail = doc.object()["detail"].toString();
                    failed->append(detail.isEmpty() ? QString("HTTP %1").arg(status) : detail);
                }
                if (*pending > 0) return;
                setMutating(false);
                m_resultLabel->setText("Your document collection changed. Search again for updated results.");
                refresh(true);
                const int added = total - failed->size() - *cancelled;
                if (*cancelled) {
                    setStatus(QString("Cancelled. Nothing was stored for %1 document(s).%2")
                                  .arg(*cancelled).arg(added ? QString(" Added %1.").arg(added) : QString()));
                } else if (failed->isEmpty()) {
                    setStatus(QString("Added %1 document(s). Ready to search.").arg(total), "ok");
                } else {
                    setStatus(QString("Added %1 of %2 documents. %3 failed: %4")
                                  .arg(added).arg(total).arg(failed->size()).arg(failed->first()), "error");
                }
            });
        beginOperation(reply);
    }
}

void RagPanel::beginOperation(QNetworkReply *reply) {
    if (!reply) return;
    m_operations.append(reply);
    if (m_runningOps++ > 0) return;
    m_progressLabel->setText("Starting…");
    m_progressBar->setRange(0, 0);
    m_cancelButton->setEnabled(true);
    m_progressBox->show();
    m_activityTimer->start();
    pollActivity();
}

void RagPanel::endOperation() {
    if (m_runningOps > 0 && --m_runningOps > 0) return;
    m_runningOps = 0;
    m_operations.clear();
    m_activityTimer->stop();
    m_progressBox->hide();
}

void RagPanel::pollActivity() {
    if (!m_client || !m_client->isReady() || m_runningOps == 0) return;
    m_client->get("/rag/activity", [this](int status, const QJsonDocument &doc) {
        if (status == 200 && m_runningOps > 0) showActivity(doc.object());
    });
}

void RagPanel::showActivity(const QJsonObject &activity) {
    static const QHash<QString, QString> phases{{"starting", "Starting"}, {"extracting", "Reading"},
        {"chunking", "Splitting"}, {"embedding", "Embedding"}, {"saving", "Saving"}};
    const QLocale en(QLocale::English);
    const int done = activity["done"].toInt(), total = activity["total"].toInt();
    const int waiting = activity["waiting"].toInt();
    QStringList parts;
    if (activity["active"].toBool()) {
        const QString name = activity["name"].toString();
        const QString phase = phases.value(activity["phase"].toString(), "Working");
        parts << (activity["kind"].toString() == "reindex"
                      ? QString("Re-indexing with %1 (%2)").arg(name, phase.toLower())
                      : QString("%1 %2").arg(phase, name).trimmed());
        if (total > 0) parts << QString("%1 of %2 chunks").arg(en.toString(done), en.toString(total));
    } else {
        parts << QStringLiteral("Waiting to start");
    }
    if (waiting > 0) parts << QString("%1 queued").arg(waiting);
    m_progressLabel->setText(parts.join(" · "));
    if (total > 0) {
        m_progressBar->setRange(0, total);
        m_progressBar->setValue(qMin(done, total));
    } else {
        m_progressBar->setRange(0, 0);
    }
}

void RagPanel::cancelOperations() {
    if (m_operations.isEmpty()) return;
    m_cancelRequested = true;
    m_cancelButton->setEnabled(false);
    setStatus("Cancelling…");
    // The backend stops within one batch (or immediately for queued requests) and stores nothing.
    const auto replies = m_operations;
    for (const auto &reply : replies)
        if (reply) reply->abort();
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
    updateEmptyState();
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
        refresh(true);   // the shown counts and model come from status, never from memory
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
    const QString search = RagParams::searchSummary(a);
    m_searchInfo->setText(search);
    m_searchInfo->setVisible(!search.isEmpty());
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
    updateEmptyState();
}

namespace {
QString memoryText(int mb) {
    return mb >= 1000 ? QString("%1 GB").arg(QString::number(mb / 1000.0, 'g', 2)) : QString("%1 MB").arg(mb);
}
}

void RagPanel::populatePicker() {
    const QSignalBlocker block(m_modelPicker);
    m_modelPicker->clear();
    const QLocale en(QLocale::English);
    for (const auto &p : m_presets) {
        QStringList detail;
        if (p.approxRamMb > 0) detail << QString("~%1 RAM").arg(memoryText(p.approxRamMb));
        if (p.maxTokens > 0) detail << QString("%1 tokens").arg(en.toString(p.maxTokens));
        m_modelPicker->addItem(p.label, p.model);
        const int row = m_modelPicker->count() - 1;
        m_modelPicker->setItemData(row, detail.join(" · "), ComboBox::DetailRole);
        m_modelPicker->setItemData(row, (QStringList{p.label} + detail).join(" · ") + "\n" + p.model, Qt::ToolTipRole);
    }
    // A model that is not a preset (set through the API) is still shown as the current one.
    if (m_active && RagParams::indexOf(m_presets, m_active->model) < 0)
        m_modelPicker->addItem(m_active->model, m_active->model);
    m_modelPicker->fitPopupToRows();
    revertPicker();
    m_modelPicker->setEnabled(!m_mutating && m_active && !m_presets.isEmpty());
}

void RagPanel::revertPicker() {
    const QSignalBlocker block(m_modelPicker);
    if (m_active) {
        const int index = RagParams::indexOf(m_presets, m_active->model);
        m_modelPicker->setCurrentIndex(index >= 0 ? index : m_modelPicker->count() - 1);
    }
    updateModelHint();
}

QString RagPanel::modelWarning(const RagParams::Preset &preset) const {
    if (!m_active) return {};
    QStringList lines;
    if (RagParams::ramIsHigh(preset, m_presets))
        lines << QString("%1 needs about %2 of RAM, more than this machine's default model. Indexing may be "
                         "refused or stopped if memory runs short.").arg(preset.label, memoryText(preset.approxRamMb));
    const QString speed = RagParams::speedWarning(preset, *m_active);
    if (!speed.isEmpty()) lines << speed;
    return lines.join("\n");
}

void RagPanel::updateModelHint() {
    const int index = m_modelPicker->currentIndex();
    const QString text = index >= 0 && index < m_presets.size() ? modelWarning(m_presets[index]) : QString();
    m_modelHint->setText(text);
    m_modelHint->setVisible(!text.isEmpty());
}

void RagPanel::onModelChosen(int index) {
    if (!m_active || index < 0 || index >= m_presets.size()) { revertPicker(); return; }
    const RagParams::Preset preset = m_presets[index];
    if (RagParams::sameModel(preset.model, m_active->model)) return;
    if (m_mutating || !m_client || !m_client->isReady()) { revertPicker(); return; }
    if (m_active->documents == 0) {
        // Nothing to re-embed: a plain config change (the backend refuses it once documents exist).
        m_client->post("/rag/config", QJsonObject{{"model", preset.model}},
            [this, label = preset.label](int status, const QJsonDocument &doc) {
                if (status != 200) {
                    const QString detail = doc.object()["detail"].toString();
                    setStatus(detail.isEmpty() ? QString("Model not changed (HTTP %1).").arg(status) : detail, "error");
                } else {
                    setStatus(QString("Now using %1. New documents will be indexed with it.").arg(label), "ok");
                }
                refresh(true);
            });
        return;
    }
    QString message = QString("Switch Knowledge to %1 (%2)? All %3 chunks from %4 document(s) will be "
                              "re-indexed. Your current index keeps working until this finishes, and "
                              "cancelling leaves it unchanged.")
                          .arg(preset.label, preset.model, QLocale(QLocale::English).toString(m_active->chunks))
                          .arg(m_active->documents);
    const QString warning = modelWarning(preset);
    if (!warning.isEmpty()) message += "\n\n" + warning;
    if (!Ui::confirmAction(this, "Re-index Knowledge?", message, "Re-index")) { revertPicker(); return; }
    startReindex(preset);
}

void RagPanel::startReindex(const RagParams::Preset &preset) {
    m_cancelRequested = false;
    setMutating(true);
    setStatus(QString("Re-indexing %1 chunks with %2…").arg(m_active ? m_active->chunks : 0).arg(preset.label));
    QNetworkReply *reply = m_client->post("/rag/reindex", QJsonObject{{"model", preset.model}},
        [this, label = preset.label](int status, const QJsonDocument &doc) {
            endOperation();
            setMutating(false);
            if (status == 200) {
                setStatus(QString("Re-indexed with %1. Ready to search.").arg(label), "ok");
            } else if (m_cancelRequested && (status == 0 || status == 499)) {
                setStatus("Re-index cancelled. The current model and index are unchanged.");
            } else {
                const QString detail = doc.object()["detail"].toString();
                setStatus(detail.isEmpty() ? QString("Re-index failed (HTTP %1).").arg(status) : detail, "error");
            }
            m_resultLabel->setText("Your document collection changed. Search again for updated results.");
            refresh(true);
        });
    beginOperation(reply);
}

void RagPanel::setMutating(bool on) {
    m_mutating = on;
    if (on) ++m_collectionRevision;
    m_addButton->setEnabled(!on);
    m_removeButton->setEnabled(!on && hasSelection());
    m_clearButton->setEnabled(!on && !m_cards.isEmpty());
    m_queryButton->setEnabled(!on && !m_queryBusy && !m_queryEdit->text().trimmed().isEmpty());
    m_modelPicker->setEnabled(!on && m_active && !m_presets.isEmpty());
    for (QSpinBox *spin : {m_chunkSizeSpin, m_chunkOverlapSpin, m_topKSpin}) spin->setEnabled(!on && m_active);
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
    updateEmptyState();
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
    m_listActions->setVisible(hasDocs);
    m_selectionHint->setVisible(hasDocs && !hasSelection());
    int enabled = 0;
    for (const auto *card : m_cards) enabled += card->sourceToggle()->isChecked();
    QString count = hasDocs ? QString("%1 document%2").arg(m_cards.size()).arg(m_cards.size() == 1 ? "" : "s")
                            : QString("Nothing indexed yet");
    if (hasDocs && m_active && m_active->chunks > 0)
        count += QString(" · %1 passage%2").arg(m_active->chunks).arg(m_active->chunks == 1 ? "" : "s");
    if (hasDocs && enabled < m_cards.size()) count += QString(" · %1 in use").arg(enabled);
    m_countLabel->setText(count);
}

void RagPanel::setStatus(const QString &text, const QString &tone) {
    m_statusLabel->setText(text);
    m_statusLabel->setVisible(!text.isEmpty());
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

void RagPanel::changeEvent(QEvent *event) {
    BasePanel::changeEvent(event);
    if (event->type() != QEvent::StyleChange && event->type() != QEvent::PaletteChange) return;
    for (auto *icon : findChildren<QLabel *>("ragEmptyIcon"))
        icon->setPixmap(Theme::coloredIcon("database", QColor(Theme::accentLabel()), 20).pixmap(20, 20));
    if (m_addButton) m_addButton->setIcon(Theme::coloredIcon("plus", QColor(Theme::accentText()), 14));
}
