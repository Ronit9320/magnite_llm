#include "widgets/overviewtile.h"

#include <QLabel>
#include <QStyle>
#include <QVBoxLayout>

OverviewTile::OverviewTile(int panel, const QString &kicker, const QString &value,
                           const QString &caption, QWidget *parent)
    : QPushButton(parent)
    , m_panel(panel)
    , m_tone()
    , m_value(new QLabel(value, this))
    , m_caption(new QLabel(caption, this))
{
    setObjectName(QStringLiteral("overviewTile"));
    setCursor(Qt::PointingHandCursor);
    setAccessibleName(kicker);
    setAccessibleDescription(value + ". " + caption);
    connect(this, &QPushButton::clicked, this, [this]() { emit activated(m_panel); });
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setMinimumHeight(84);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(3);

    auto *kickerLabel = new QLabel(kicker, this);
    kickerLabel->setObjectName(QStringLiteral("tileKicker"));

    m_value->setObjectName(QStringLiteral("tileValue"));
    m_value->setWordWrap(true);
    m_value->setToolTip(value);

    m_caption->setObjectName(QStringLiteral("tileCaption"));
    m_caption->setWordWrap(true);

    for (auto *label : {kickerLabel, m_value, m_caption})
        label->setAttribute(Qt::WA_TransparentForMouseEvents);

    layout->addWidget(kickerLabel);
    layout->addWidget(m_value);
    layout->addSpacing(2);
    layout->addWidget(m_caption);
    layout->addStretch(1);
}

void OverviewTile::setState(const QString &value, const QString &caption,
                            const QString &tone) {
    setAccessibleDescription(value + ". " + caption);
    m_value->setText(value);
    m_value->setToolTip(value);
    m_caption->setText(caption);
    if (m_tone == tone) return;
    m_tone = tone;
    setProperty("tone", tone.isEmpty() ? QVariant() : QVariant(tone));
    style()->unpolish(this);
    style()->polish(this);
    update();
}

