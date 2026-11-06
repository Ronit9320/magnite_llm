#include "widgets/sidebarstate.h"

#include <QSettings>

SidebarState SidebarState::load() {
    // QVariant::toBool() treats any non-empty text as true; only a real true (or the text "true"/"1") collapses.
    const QVariant stored = QSettings().value(kSettingsKey, false);
    const QString text = stored.toString().trimmed().toLower();
    return SidebarState(text == QLatin1String("true") || text == QLatin1String("1"));
}

void SidebarState::save() const {
    QSettings().setValue(kSettingsKey, m_collapsed);
}

bool SidebarState::toggle() {
    m_collapsed = !m_collapsed;
    return m_collapsed;
}

QString SidebarState::toggleTooltip() const {
    return m_collapsed ? QStringLiteral("Expand sidebar · Ctrl+B")
                       : QStringLiteral("Collapse sidebar · Ctrl+B");
}
