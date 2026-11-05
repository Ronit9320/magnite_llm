#include "widgets/versionswitcher.h"

#include "theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>

VersionSwitcher::VersionSwitcher(QWidget *parent)
    : QWidget(parent)
    , m_prev(nullptr)
    , m_next(nullptr)
    , m_label(nullptr)
{
    setObjectName(QStringLiteral("versionSwitcher"));
    auto *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(2);

    auto makeArrow = [this](const QString &objectName, const QString &icon, const QString &accessible) {
        auto *button = new QPushButton(this);
        button->setObjectName(objectName);
        button->setIcon(Theme::coloredIcon(icon, QColor(Theme::textMuted()), 14));
        button->setFixedSize(22, 22);
        button->setFlat(true);
        button->setCursor(Qt::PointingHandCursor);
        button->setAccessibleName(accessible);
        button->setToolTip(accessible);
        return button;
    };
    m_prev = makeArrow(QStringLiteral("versionPrev"), QStringLiteral("chevron-left"), QStringLiteral("Previous version"));
    m_next = makeArrow(QStringLiteral("versionNext"), QStringLiteral("chevron-right"), QStringLiteral("Next version"));
    m_label = new QLabel(this);
    m_label->setObjectName(QStringLiteral("versionLabel"));

    layout->addWidget(m_prev);
    layout->addWidget(m_label);
    layout->addWidget(m_next);

    connect(m_prev, &QPushButton::clicked, this, [this]() { emit step(-1); });
    connect(m_next, &QPushButton::clicked, this, [this]() { emit step(+1); });
    refresh();
}

void VersionSwitcher::setVersion(int index, int total) {
    m_total = qMax(total, 1);
    m_index = qBound(0, index, m_total - 1);
    refresh();
}

void VersionSwitcher::setEnabledForUse(bool enabled) {
    m_enabled = enabled;
    refresh();
}

void VersionSwitcher::refresh() {
    m_label->setText(QStringLiteral("%1 / %2").arg(m_index + 1).arg(m_total));
    m_prev->setEnabled(m_enabled && m_index > 0);
    m_next->setEnabled(m_enabled && m_index < m_total - 1);
    setHidden(m_total <= 1);
}
