#include "panels/adapterspanel.h"

#include "backend_client.h"
#include "panels/adaptercard.h"
#include "widgets/flowlayout.h"

#include <QFileDialog>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

AdaptersPanel::AdaptersPanel(QWidget *parent)
    : BasePanel("Adapters", parent)
    , m_client(nullptr)
    , m_scrollArea(nullptr)
    , m_flowLayout(nullptr)
    , m_cardHost(nullptr)
    , m_emptyState(nullptr)
    , m_statusLabel(nullptr)
{
    setHeaderSubtitle(
        "Manage trained adapters: activate one for inference, export/import, "
        "or delete. Activating hot-switches the Chat panel's active model.");
    setupUi();
}

QWidget *AdaptersPanel::makeCardHost() {
    m_cardHost = new QWidget(this);
    m_flowLayout = new FlowLayout(m_cardHost, 24, 24);
    m_flowLayout->setContentsMargins(2, 2, 2, 2);
    m_cardHost->setLayout(m_flowLayout);
    return m_cardHost;
}

void AdaptersPanel::setupUi() {
    auto *layout = contentLayout();

    m_scrollArea = new QScrollArea(this);
    m_scrollArea->setWidgetResizable(true);
    m_scrollArea->setFrameShape(QFrame::NoFrame);
    m_scrollArea->setObjectName("cardScroll");
    m_scrollArea->viewport()->setAutoFillBackground(false);
    m_scrollArea->setWidget(makeCardHost());
    m_scrollArea->setVisible(false);
    layout->addWidget(m_scrollArea, 1);

    // Empty state: centered hint + Import (import is the only adapter action
    // usable with no cards present).
    m_emptyState = new QWidget(this);
    auto *emptyLayout = new QVBoxLayout(m_emptyState);
    emptyLayout->setContentsMargins(24, 24, 24, 24);
    emptyLayout->setSpacing(14);
    emptyLayout->setAlignment(Qt::AlignCenter);
    auto *hint = new QLabel(
        "No adapters yet. Train one in the Training panel.\n\n"
        "Have an export archive? Import it below.",
        m_emptyState);
    hint->setObjectName("bodyLabel");
    hint->setAlignment(Qt::AlignCenter);
    hint->setWordWrap(true);
    emptyLayout->addWidget(hint);
    auto *importButton = new QPushButton("Import adapter...", m_emptyState);
    importButton->setObjectName("secondaryBtn");
    importButton->setCursor(Qt::PointingHandCursor);
    emptyLayout->addWidget(importButton, 0, Qt::AlignHCenter);
    m_emptyState->setLayout(emptyLayout);
    layout->addWidget(m_emptyState, 1);

    m_statusLabel = new QLabel(this);
    m_statusLabel->setObjectName("statusLabel");
    m_statusLabel->setWordWrap(true);
    layout->addWidget(m_statusLabel);
    m_statusLabel->setText("No adapters yet. Train one in the Training panel.");

    connect(importButton, &QPushButton::clicked, this, &AdaptersPanel::onImportAdapter);
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
            setStatus(QString("Failed to load adapters (HTTP %1).").arg(status), "error");
            return;
        }
        clearCards();
        const QJsonArray arr = doc.array();
        for (const auto &value : arr) {
            if (!value.isObject()) continue;
            const QJsonObject adapter = value.toObject();
            auto *card = new AdapterCard(adapter, m_cardHost);
            connect(card, &AdapterCard::activateRequested, this, &AdaptersPanel::onActivate);
            connect(card, &AdapterCard::exportRequested, this, &AdaptersPanel::onExportAdapter);
            connect(card, &AdapterCard::deleteRequested, this, &AdaptersPanel::onDeleteAdapter);
            connect(card, &AdapterCard::importRequested, this, &AdaptersPanel::onImportAdapter);
            card->setActive(card->id() == m_activeAdapterId);
            m_flowLayout->addWidget(card);
            m_cards.append(card);
        }
        m_flowLayout->invalidate();
        m_cardHost->updateGeometry();

        const bool hasAdapter = !arr.isEmpty();
        m_scrollArea->setVisible(hasAdapter);
        m_emptyState->setVisible(!hasAdapter);
        if (!hasAdapter) {
            setStatus("No adapters yet. Train one in the Training panel.");
        } else if (m_activeAdapterId.isEmpty()) {
            setStatus(QString("%1 adapter(s). Press Activate on one to serve Chat.")
                          .arg(arr.size()));
        } else {
            setStatus(QString("%1 adapter(s). The glowing card serves Chat right now.")
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

void AdaptersPanel::onActivate(AdapterCard *card) {
    if (!card || !m_client || !m_client->isReady()) return;
    const QString id = card->id();
    const bool willActivate = (id != m_activeAdapterId);
    QJsonObject body;
    body["adapter_id"] = willActivate ? id : "";
    setStatus(willActivate ? "Activating adapter (loading base model + PEFT weights)..."
                           : "Clearing active adapter...");
    m_client->post("/inference/adapter", body, [this, id](int status, const QJsonDocument &doc) {
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

void AdaptersPanel::onDeleteAdapter(AdapterCard *card) {
    const QString id = card ? card->id() : QString();
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

void AdaptersPanel::onExportAdapter(AdapterCard *card) {
    const QString id = card ? card->id() : QString();
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
    m_client->post("/adapters/import", body, [this](int status, const QJsonDocument &doc) {
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

void AdaptersPanel::clearCards() {
    while (QLayoutItem *item = m_flowLayout->takeAt(0)) {
        if (QWidget *w = item->widget()) {
            w->hide();             // stop painting immediately
            w->setParent(nullptr); // detach from host so it can't overlap new cards
            w->deleteLater();
        }
        delete item;
    }
    m_cards.clear();
    m_flowLayout->invalidate();
    m_cardHost->updateGeometry();
}

void AdaptersPanel::setStatus(const QString &text, const QString &tone) {
    m_statusLabel->setText(text);
    applyStatusTone(m_statusLabel, tone);
}