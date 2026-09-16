#include "panels/ragpanel.h"

#include "panels/ragdoccard.h"

#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

RagPanel::RagPanel(QWidget *parent)
    : BasePanel("RAG", parent)
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
    , m_statusLabel(nullptr)
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

    // Document list — full-width glass cards in a transparent scroll area.
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
    layout->addWidget(m_docScroll, 1);

    // Empty state (shown instead of the scroll area when no docs are added).
    m_emptyState = new QWidget(this);
    auto *emptyLayout = new QVBoxLayout(m_emptyState);
    emptyLayout->setContentsMargins(24, 24, 24, 24);
    emptyLayout->setAlignment(Qt::AlignCenter);
    auto *hint = new QLabel("No documents ingested yet.\nAdd PDFs, text, markdown or HTML above.", m_emptyState);
    hint->setObjectName("mutedLabel");
    hint->setAlignment(Qt::AlignCenter);
    hint->setWordWrap(true);
    emptyLayout->addWidget(hint);
    m_emptyState->setLayout(emptyLayout);
    layout->addWidget(m_emptyState, 1);

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
    layout->addWidget(configCard);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName("statusLabel");
    layout->addWidget(m_statusLabel);
    m_statusLabel->setText("Ready. RAG engine lands in Phase 6.");

    connect(m_addButton, &QPushButton::clicked, this, &RagPanel::onAddDocuments);
    connect(m_removeButton, &QPushButton::clicked, this, &RagPanel::onRemoveDocument);
    connect(m_clearButton, &QPushButton::clicked, this, &RagPanel::onClearAll);

    updateEmptyState();
}

void RagPanel::onAddDocuments() {
    const QStringList files = QFileDialog::getOpenFileNames(
        this, "Select documents",
        QString(),
        "Documents (*.pdf *.txt *.md *.html);;All Files (*)");
    for (const QString &f : files) {
        auto *card = new RagDocCard(f, m_docHost);
        connect(card, &RagDocCard::cardClicked, this, &RagPanel::onCardClicked);
        m_docLayout->addWidget(card);
        m_cards.append(card);
    }
    m_docLayout->invalidate();
    m_docHost->updateGeometry();
    updateEmptyState();
    if (!files.isEmpty()) {
        setStatus(QString("%1 document(s) queued for ingestion (Phase 6).")
                      .arg(files.size()));
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

void RagPanel::onRemoveDocument() {
    int removed = 0;
    QList<RagDocCard *> toRemove;
    for (RagDocCard *c : m_cards) {
        if (c->isCardSelected())
            toRemove.append(c);
    }
    for (RagDocCard *c : toRemove) {
        m_docLayout->removeWidget(c);
        c->hide();
        c->setParent(nullptr);
        c->deleteLater();
        m_cards.removeOne(c);
        ++removed;
    }
    m_docLayout->invalidate();
    m_docHost->updateGeometry();
    m_removeButton->setEnabled(false);
    updateEmptyState();
    setStatus(removed ? QString("%1 document(s) removed.").arg(removed)
                      : "No documents selected.");
}

void RagPanel::onClearAll() {
    clearCards();
    setStatus("Cleared all documents.");
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

void RagPanel::setStatus(const QString &text) {
    m_statusLabel->setText(text);
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
    auto o = doc.object();
    if (o.contains("chunkSize")) m_chunkSizeSpin->setValue(o["chunkSize"].toInt());
    if (o.contains("chunkOverlap")) m_chunkOverlapSpin->setValue(o["chunkOverlap"].toInt());
    if (o.contains("topK")) m_topKSpin->setValue(o["topK"].toInt());
}