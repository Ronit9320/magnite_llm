#include "panels/ragpanel.h"

#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

RagPanel::RagPanel(QWidget *parent)
    : BasePanel("RAG", parent)
    , m_docList(nullptr)
    , m_addButton(nullptr)
    , m_removeButton(nullptr)
    , m_clearButton(nullptr)
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
    m_removeButton = new QPushButton("Remove Selected", this);
    m_removeButton->setEnabled(false);
    m_clearButton = new QPushButton("Clear All", this);
    btnRow->addWidget(m_addButton);
    btnRow->addWidget(m_removeButton);
    btnRow->addWidget(m_clearButton);
    btnRow->addStretch();
    layout->addLayout(btnRow);

    m_docList = new QListWidget(this);
    layout->addWidget(m_docList, 1);
    QLabel *emptyHint = new QLabel("No documents ingested yet.", this);
    emptyHint->setStyleSheet("color: #888;");
    layout->addWidget(emptyHint);

    connect(m_docList, &QListWidget::itemSelectionChanged, this, [this]() {
        m_removeButton->setEnabled(!m_docList->selectedItems().isEmpty());
    });

    auto *retrievalBox = new QGroupBox("Retrieval Configuration", this);
    auto *form = new QFormLayout(retrievalBox);

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

    layout->addWidget(retrievalBox);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet("color: #666;");
    layout->addWidget(m_statusLabel);
    m_statusLabel->setText("Ready. RAG engine lands in Phase 6.");

    connect(m_addButton, &QPushButton::clicked, this, &RagPanel::onAddDocuments);
    connect(m_removeButton, &QPushButton::clicked, this, &RagPanel::onRemoveDocument);
    connect(m_clearButton, &QPushButton::clicked, this, &RagPanel::onClearAll);
}

void RagPanel::onAddDocuments() {
    QStringList files = QFileDialog::getOpenFileNames(
        this, "Select documents",
        QString(),
        "Documents (*.pdf *.txt *.md *.html);;All Files (*)");
    for (const QString &f : files) {
        m_docList->addItem(f);
    }
    if (!files.isEmpty()) {
        setStatus(QString("%1 document(s) queued for ingestion (Phase 6).")
                      .arg(files.size()));
    }
}

void RagPanel::onRemoveDocument() {
    for (auto *item : m_docList->selectedItems()) {
        delete m_docList->takeItem(m_docList->row(item));
    }
    setStatus("Document(s) removed.");
}

void RagPanel::onClearAll() {
    m_docList->clear();
    setStatus("Cleared all documents.");
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
