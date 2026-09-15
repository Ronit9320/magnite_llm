#include "panels/adapterspanel.h"

#include "backend_client.h"
#include "theme.h"

#include <QBrush>
#include <QColor>
#include <QFileDialog>
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
    , m_exportButton(nullptr)
    , m_importButton(nullptr)
    , m_deleteButton(nullptr)
    , m_statusLabel(nullptr)
{
    setHeaderSubtitle(
        "Manage trained adapters: activate one for inference, export/import, "
        "or delete. Activating hot-switches the Chat panel's active model.");
    setupUi();
}

void AdaptersPanel::setupUi() {
    auto *layout = contentLayout();

    m_table = new QTableWidget(this);
    m_table->setColumnCount(7);
    m_table->setHorizontalHeaderLabels(
        {"Name", "Base Model", "Rank", "Method", "Size", "Trained", "Active"});
    m_table->horizontalHeader()->setStretchLastSection(true);
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    for (int i = 1; i < 7; ++i)
        m_table->horizontalHeader()->setSectionResizeMode(i, QHeaderView::ResizeToContents);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    layout->addWidget(m_table, 1);

    auto *btnRow = new QHBoxLayout();
    m_activateButton = new QPushButton("Activate", this);
    m_activateButton->setObjectName("primaryBtn");
    m_exportButton = new QPushButton("Export", this);
    m_exportButton->setObjectName("secondaryBtn");
    m_importButton = new QPushButton("Import...", this);
    m_importButton->setObjectName("secondaryBtn");
    m_deleteButton = new QPushButton("Delete", this);
    m_deleteButton->setObjectName("destructiveBtn");
    m_activateButton->setEnabled(false);
    m_exportButton->setEnabled(false);
    m_deleteButton->setEnabled(false);
    btnRow->addWidget(m_activateButton);
    btnRow->addWidget(m_exportButton);
    btnRow->addWidget(m_importButton);
    btnRow->addStretch();
    btnRow->addWidget(m_deleteButton);
    layout->addLayout(btnRow);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName("statusLabel");
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);
    m_statusLabel->setText("No adapters yet. Train one in the Training panel.");

    connect(m_table, &QTableWidget::itemSelectionChanged, this, [this]() {
        bool has = !m_table->selectedItems().isEmpty();
        m_activateButton->setEnabled(has);
        m_exportButton->setEnabled(has);
        m_deleteButton->setEnabled(has);
    });

    connect(m_activateButton, &QPushButton::clicked, this, &AdaptersPanel::onActivate);
    connect(m_exportButton, &QPushButton::clicked, this, &AdaptersPanel::onExportAdapter);
    connect(m_importButton, &QPushButton::clicked, this, &AdaptersPanel::onImportAdapter);
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
    loadActiveAdapter();
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
            auto *active = new QTableWidgetItem(
                a["id"].toString() == m_activeAdapterId ? "Active" : "");
            active->setForeground(QBrush(QColor(Theme::statusSuccess())));
            m_table->setItem(row, 6, active);
        }
        m_activateButton->setEnabled(false);
        m_exportButton->setEnabled(false);
        m_deleteButton->setEnabled(false);
        const bool hasAdapter = !arr.isEmpty();
        if (!hasAdapter) {
            setStatus("No adapters yet. Train one in the Training panel.");
        } else if (m_activeAdapterId.isEmpty()) {
            setStatus(QString("%1 adapter(s). Select one and press Activate.")
                          .arg(arr.size()));
        } else {
            setStatus(QString("%1 adapter(s). \"Active\" marks the one serving Chat right now.")
                          .arg(arr.size()));
        }
    });
}

void AdaptersPanel::loadActiveAdapter() {
    if (!m_client || !m_client->isReady()) return;
    m_client->get("/inference/status", [this](int status, const QJsonDocument &doc) {
        const QString id = (status == 200) ? doc.object()["active_adapter_id"].toString()
                                           : QString();
        if (m_activeAdapterId != id) {
            m_activeAdapterId = id;
            refresh();
        }
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
    const QString id = selectedId();
    if (id.isEmpty() || !m_client || !m_client->isReady()) return;

    const bool willActivate = (id != m_activeAdapterId);
    QJsonObject body;
    body["adapter_id"] = willActivate ? id : "";
    setStatus(willActivate ? "Activating adapter (loading base model + PEFT weights)..."
                           : "Clearing active adapter...");
    m_client->post("/inference/adapter", body,
                   [this, id](int status, const QJsonDocument &doc) {
        if (status == 200) {
            const QJsonObject obj = doc.object();
            m_activeAdapterId = obj["active_adapter_id"].toString();
            setStatus(obj.isEmpty() || obj["active_adapter_id"].isUndefined()
                          ? "Adapter cleared."
                          : QString("Active adapter: %1").arg(obj["active_adapter"].toString()));
            refresh();
            emit adapterActivated();
        } else {
            const QString err = doc.object().contains("detail")
                                    ? doc.object()["detail"].toString()
                                    : QString("HTTP %1").arg(status);
            setStatus("Activate failed: " + err, "error");
        }
    });
}

void AdaptersPanel::onDeleteAdapter() {
    const QString id = selectedId();
    if (id.isEmpty() || !m_client || !m_client->isReady()) return;
    m_client->remove("/adapters/" + id, [this, id](int status, const QJsonDocument &) {
        if (status == 204 || status == 404) {
            if (m_activeAdapterId == id) {
                m_activeAdapterId.clear();
                emit adapterActivated();
            }
            refresh();
        } else {
            setStatus(QString("Delete failed (HTTP %1).").arg(status), "error");
        }
    });
}

void AdaptersPanel::onExportAdapter() {
    const QString id = selectedId();
    if (id.isEmpty() || !m_client || !m_client->isReady()) return;
    setStatus("Exporting adapter...");
    m_client->post(QString("/adapters/export/%1").arg(id), QJsonObject(),
                   [this, id](int status, const QJsonDocument &doc) {
        if (status == 200) {
            const QJsonObject obj = doc.object();
            const double size = obj["size_bytes"].toDouble() / 1024.0 / 1024.0;
            setStatus(QString("Exported '%1' (%2 MB). Download at %3")
                          .arg(id, QString::number(size, 'f', 2),
                               obj["download_path"].toString()));
        } else {
            setStatus(QString("Export failed (HTTP %1).").arg(status), "error");
        }
    });
}

void AdaptersPanel::onImportAdapter() {
    const QString path = QFileDialog::getOpenFileName(
        this, "Import Magnite Adapter Export", QString(), "Magnite adapter (*.zip)");
    if (path.isEmpty() || !m_client || !m_client->isReady()) return;
    QJsonObject body;
    body["path"] = path;
    setStatus("Importing adapter...");
    m_client->post("/adapters/import", body,
                   [this](int status, const QJsonDocument &doc) {
        if (status == 201) {
            const QJsonObject obj = doc.object();
            setStatus(QString("Imported adapter '%1'.").arg(obj["name"].toString()));
            refresh();
        } else {
            const QString err = doc.object().contains("detail")
                                    ? doc.object()["detail"].toString()
                                    : QString("HTTP %1").arg(status);
            setStatus("Import failed: " + err, "error");
        }
    });
}

void AdaptersPanel::setStatus(const QString &text, const QString &tone) {
    m_statusLabel->setText(text);
    applyStatusTone(m_statusLabel, tone);
}