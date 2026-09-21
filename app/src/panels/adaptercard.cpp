#include "panels/adaptercard.h"

#include "theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

AdapterCard::AdapterCard(const QJsonObject &data, QWidget *parent)
    : QFrame(parent)
    , m_activateButton(nullptr)
    , m_id(data.value("id").toString())
{
    setObjectName(QStringLiteral("adapterCard"));
    setMinimumWidth(300);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 24, 24, 24);
    layout->setSpacing(16);

    // Row 1: name (Space Grotesk) + base-model badge.
    auto *nameRow = new QHBoxLayout();
    nameRow->setSpacing(8);
    auto *name = new QLabel(m_id, this);
    name->setObjectName(QStringLiteral("cardTitle"));
    name->setWordWrap(true);
    nameRow->addWidget(name, 1);
    const QString baseModel = data.value("base_model").toString();
    if (!baseModel.isEmpty()) {
        auto *badge = new QLabel(baseModel, this);
        badge->setObjectName(QStringLiteral("archBadge"));
        badge->setToolTip("Base model");
        nameRow->addWidget(badge, 0, Qt::AlignTop);
    }
    layout->addLayout(nameRow);

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
        auto *date = new QLabel(trainedAt, this);
        date->setObjectName(QStringLiteral("cardMeta"));
        metaRow->addWidget(date);
    }
    metaRow->addStretch(1);
    layout->addLayout(metaRow);

    // Row 3: action buttons (Activate primary; Export/Import/Delete secondary).
    auto *btnRow = new QHBoxLayout();
    btnRow->setSpacing(6);
    m_activateButton = new QPushButton(QStringLiteral("Activate"), this);
    m_activateButton->setObjectName(QStringLiteral("cardPrimary"));
    m_activateButton->setCursor(Qt::PointingHandCursor);
    auto *exportButton = new QPushButton(QStringLiteral("Export"), this);
    exportButton->setObjectName(QStringLiteral("cardBtn"));
    exportButton->setCursor(Qt::PointingHandCursor);
    auto *importButton = new QPushButton(QStringLiteral("Import"), this);
    importButton->setObjectName(QStringLiteral("cardBtn"));
    importButton->setCursor(Qt::PointingHandCursor);
    auto *deleteButton = new QPushButton(QStringLiteral("Delete"), this);
    deleteButton->setObjectName(QStringLiteral("cardDanger"));
    deleteButton->setCursor(Qt::PointingHandCursor);
    btnRow->addWidget(m_activateButton);
    btnRow->addWidget(exportButton);
    btnRow->addWidget(importButton);
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
    connect(importButton, &QPushButton::clicked, this, &AdapterCard::importRequested);
}

void AdapterCard::setActive(bool active) {
    if (m_active == active) return;
    m_active = active;
    setProperty("active", active);
    style()->unpolish(this);
    style()->polish(this);
    if (m_activateButton)
        m_activateButton->setText(active ? QStringLiteral("Clear") : QStringLiteral("Activate"));
    update();
}

void AdapterCard::paintEvent(QPaintEvent *event) {
    if (m_active) {
        // Accent rim-glow: a radial blue gradient feathered toward the edge,
        // painted before the QSS border so the active card visibly glows.
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        QPainterPath path;
        path.addRoundedRect(QRectF(rect())
                                .adjusted(0.5, 0.5, -0.5, -0.5),
                            Theme::cornerRadius(), Theme::cornerRadius());
        QRadialGradient glow(rect().center(), width() / 2.0);
        auto accentAt = [](int alpha) {
            QColor c(Theme::accentPrimary());
            c.setAlpha(alpha);
            return c;
        };
        glow.setColorAt(0.0, accentAt(16));
        glow.setColorAt(0.65, accentAt(42));
        glow.setColorAt(1.0, accentAt(92));
        painter.fillPath(path, glow);
    }
    QFrame::paintEvent(event);
}