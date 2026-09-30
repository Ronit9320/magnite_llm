#include "panels/adaptercard.h"


#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>
#include <QDateTime>

AdapterCard::AdapterCard(const QJsonObject &data, QWidget *parent)
    : QFrame(parent)
    , m_activateButton(nullptr)
    , m_id(data.value("id").toString())
{
    setObjectName(QStringLiteral("adapterCard"));
    setMinimumWidth(230);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(16,14,16,14);
    row->setSpacing(16);
    auto *layout = new QVBoxLayout();
    row->addLayout(layout,1);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    // Row 1: name (Space Grotesk) + base-model badge.
    auto *nameRow = new QHBoxLayout();
    nameRow->setSpacing(8);
    auto *name = new QLabel(data.value("name").toString(m_id), this);
    name->setObjectName(QStringLiteral("cardTitle"));
    name->setWordWrap(true);
    name->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    name->setToolTip(m_id);
    nameRow->addWidget(name, 1);
    m_activeLabel = new QLabel("Active in Playground",this);
    m_activeLabel->setObjectName("activeBadge");
    m_activeLabel->hide();
    layout->addWidget(m_activeLabel);
    layout->addLayout(nameRow);
    const QString baseModel = data.value("base_model").toString();
    if (!baseModel.isEmpty()) {
        auto *badge = new QLabel("Base model · "+baseModel, this);
        badge->setObjectName(QStringLiteral("cardMeta"));
        badge->setToolTip("Base model: " + baseModel);
        badge->setWordWrap(true);
        badge->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
        layout->addWidget(badge);
    }

    // Row 2: rank, method pill, size, trained date.
    auto *metaRow = new QHBoxLayout();
    metaRow->setSpacing(6);
    const int rank = data.value("rank").toInt();
    if (rank > 0) {
        auto *rankLabel = new QLabel(QStringLiteral("r=%1").arg(rank), this);
        rankLabel->setObjectName(QStringLiteral("adapterPill"));
        metaRow->addWidget(rankLabel);
    }
    const QString method = data.value("method").toString().toUpper();
    if (!method.isEmpty()) {
        auto *methodLabel = new QLabel(method, this);
        methodLabel->setObjectName(QStringLiteral("adapterPill"));
        metaRow->addWidget(methodLabel);
    }
    auto *dot = new QLabel(QStringLiteral("·"), this);
    dot->setObjectName(QStringLiteral("cardMeta"));
    metaRow->addWidget(dot);
    auto *size = new QLabel(
        QStringLiteral("%1 MB").arg(data.value("size_mb").toDouble(), 0, 'f', 1), this);
    size->setObjectName(QStringLiteral("cardMeta"));
    metaRow->addWidget(size);
    const QString trainedAt = data.value("trained_at").toString();
    if (!trainedAt.isEmpty()) {
        auto *dateDot = new QLabel(QStringLiteral("·"), this);
        dateDot->setObjectName(QStringLiteral("cardMeta"));
        metaRow->addWidget(dateDot);
        const auto timestamp = QDateTime::fromString(trainedAt,Qt::ISODate);
        auto *date = new QLabel(timestamp.isValid() ? timestamp.date().toString("dd MMM yyyy") : trainedAt.left(10), this);
        date->setToolTip(trainedAt);
        date->setObjectName(QStringLiteral("cardMeta"));
        metaRow->addWidget(date);
    }
    metaRow->addStretch(1);
    layout->addLayout(metaRow);

    // Row 3: activation, export and deletion actions.
    auto *btnRow = new QHBoxLayout();
    btnRow->setSpacing(6);
    m_activateButton = new QPushButton(QStringLiteral("Activate"), this);
    m_activateButton->setObjectName(QStringLiteral("cardPrimary"));
    m_activateButton->setCursor(Qt::PointingHandCursor);
    auto *exportButton = new QPushButton(QStringLiteral("Export"), this);
    exportButton->setObjectName(QStringLiteral("cardBtn"));
    exportButton->setCursor(Qt::PointingHandCursor);
    auto *deleteButton = new QPushButton(QStringLiteral("Delete"), this);
    deleteButton->setObjectName(QStringLiteral("cardDanger"));
    deleteButton->setCursor(Qt::PointingHandCursor);
    btnRow->addWidget(m_activateButton);
    btnRow->addWidget(exportButton);
    btnRow->addWidget(deleteButton);
    btnRow->addStretch(1);
    layout->addLayout(btnRow);

    connect(m_activateButton, &QPushButton::clicked, this, [this]() {
        emit activateRequested(this);
    });
    connect(exportButton, &QPushButton::clicked, this, [this]() {
        emit exportRequested(this);
    });
    connect(deleteButton, &QPushButton::clicked, this, [this]() {
        emit deleteRequested(this);
    });
    for (auto *label : findChildren<QLabel *>()) label->setTextFormat(Qt::PlainText);
    m_activateButton->setAccessibleName("Activate adapter "+m_id);
    exportButton->setAccessibleName("Export adapter "+m_id);
    deleteButton->setAccessibleName("Delete adapter "+m_id);
}

void AdapterCard::setActive(bool active) {
    if (m_active == active) return;
    m_active = active;
    m_activeLabel->setVisible(active);
    setAccessibleDescription(active ? "Active in Playground" : "Available adapter");
    setProperty("active", active);
    style()->unpolish(this);
    style()->polish(this);
    if (m_activateButton)
        m_activateButton->setText(active ? QStringLiteral("Deactivate") : QStringLiteral("Activate"));
    update();
}
