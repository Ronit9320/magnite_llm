#include "panels/adapterspanel.h"

#include "backend_client.h"

#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

AdaptersPanel::AdaptersPanel(QWidget *parent)
    : BasePanel("Adapters", parent)
    , m_client(nullptr)
    , m_table(nullptr)
    , m_activateButton(nullptr)
    , m_deleteButton(nullptr)
    , m_statusLabel(nullptr)
{
    setHeaderSubtitle(
        "Manage trained LoRA adapters and switch behavior on the fly without "
        "reloading the base model.");
    setupUi();
}

void AdaptersPanel::setupUi() {
    auto *layout = contentLayout();

    m_table = new QTableWidget(this);
    m_table->setColumnCount(6);
    m_table->setHorizontalHeaderLabels(
        {"Name", "Base Model", "Rank", "Method", "Size", "Trained"});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int i = 1; i < 6; ++i)
        m_table->horizontalHeader()->setSectionResizeMode(i, QHeaderView::ResizeToContents);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(m_table, 1);

    auto *btnRow = new QHBoxLayout();
    m_activateButton = new QPushButton("Activate Adapter", this);
    m_deleteButton = new QPushButton("Delete", this);
    m_activateButton->setEnabled(false);
    m_deleteButton->setEnabled(false);
    btnRow->addWidget(m_activateButton);
    btnRow->addWidget(m_deleteButton);
    btnRow->addStretch();
    layout->addLayout(btnRow);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setStyleSheet("color: #666;");
    layout->addWidget(m_statusLabel);
    m_statusLabel->setText("No adapters yet. Train one in the Training panel.");

    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this]() {
        bool has = !m_table->selectedItems().isEmpty();
        m_activateButton->setEnabled(has);
        m_deleteButton->setEnabled(has);
    });

    connect(m_activateButton, &QPushButton::clicked, this, &AdaptersPanel::onActivate);
    connect(m_deleteButton, &QPushButton::clicked, this, &AdaptersPanel::onDeleteAdapter);
}

void AdaptersPanel::setBackendClient(BackendClient *client) {
    m_client = client;
    if (client && client->isReady()) {
        refresh();
    }
}

void AdaptersPanel::refresh() {
    if (!m_client || !m_client->isReady()) {
        setStatus("Backend not connected.");
        return;
    }
    m_client->get("/adapters", [this](int status, const QJsonDocument &doc) {
        if (status != 200) {
            setStatus(QString("Failed to load adapters (HTTP %1).").arg(status));
            return;
        }
        const QJsonArray arr = doc.array();
        m_table->setRowCount(arr.size());
        for (int row = 0; row < arr.size(); ++row) {
            const QJsonObject a = arr.at(row).toObject();
            auto *name = new QTableWidgetItem(a["name"].toString());
            name->setData(Qt::UserRole, a["id"].toString());
            m_table->setItem(row, 0, name);
            m_table->setItem(row, 1, new QTableWidgetItem(a["base_model"].toString()));
            m_table->setItem(row, 2, new QTableWidgetItem(QString::number(a["rank"].toInt())));
            m_table->setItem(row, 3, new QTableWidgetItem(a["method"].toString().toUpper()));
            m_table->setItem(row, 4, new QTableWidgetItem(
                QString("%1 MB").arg(a["size_mb"].toDouble(), 0, 'f', 1)));
            m_table->setItem(row, 5, new QTableWidgetItem(a["trained_at"].toString()));
        }
        m_activateButton->setEnabled(false);
        m_deleteButton->setEnabled(false);
        setStatus(arr.isEmpty() ? "No adapters yet. Train one in the Training panel."
                                : QString("%1 adapter(s).").arg(arr.size()));
    });
}

QString AdaptersPanel::selectedId() const {
    const auto items = m_table->selectedItems();
    if (items.isEmpty()) return QString();
    return items.first()->data(Qt::UserRole).toString();
}

void AdaptersPanel::onActivate() {
    int row = m_table->currentRow();
    if (row < 0) return;
    auto *nameItem = m_table->item(row, 0);
    setStatus(QString("Activating \"%1\" (hot-switching lands in Phase 5).")
                  .arg(nameItem ? nameItem->text() : "?"));
}

void AdaptersPanel::onDeleteAdapter() {
    const QString id = selectedId();
    if (id.isEmpty() || !m_client || !m_client->isReady()) return;
    m_client->remove("/adapters/" + id, [this](int status, const QJsonDocument &) {
        if (status == 204 || status == 404) {
            refresh();
        } else {
            setStatus(QString("Delete failed (HTTP %1).").arg(status));
        }
    });
}

void AdaptersPanel::setStatus(const QString &text) {
    m_statusLabel->setText(text);
}