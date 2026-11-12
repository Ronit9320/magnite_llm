#pragma once

#include <QString>

// The decisions behind the collapsible left menu: collapsed or not, how wide it is, what shows,
// and the stored choice. No widgets, so the UI check can test it on its own.
class SidebarState {
public:
    static constexpr int kExpandedWidth = 220;
    static constexpr int kCollapsedWidth = 52;
    static constexpr const char *kSettingsKey = "appearance/sidebarCollapsed";

    explicit SidebarState(bool collapsed = false) : m_collapsed(collapsed) {}

    // A missing or unreadable stored value means expanded.
    static SidebarState load();
    void save() const;

    bool collapsed() const { return m_collapsed; }
    void setCollapsed(bool collapsed) { m_collapsed = collapsed; }
    // Flips the state and returns the new collapsed value.
    bool toggle();

    int width() const { return m_collapsed ? kCollapsedWidth : kExpandedWidth; }
    // The wordmark, the small tags and the text on the nav buttons.
    bool showLabels() const { return !m_collapsed; }
    // The saved chats list: only while expanded and Playground is the open workspace.
    bool showHistory(bool playgroundOpen) const { return !m_collapsed && playgroundOpen; }
    // The small + under Playground: only while collapsed and Playground is the open workspace.
    bool showNewChatButton(bool playgroundOpen) const { return m_collapsed && playgroundOpen; }
    QString toggleTooltip() const;

private:
    bool m_collapsed;
};
