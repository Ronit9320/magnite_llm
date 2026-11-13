#pragma once

#include "widgets/sidebarstate.h"

#include <QList>
#include <QString>
#include <QWidget>

class ChatHistoryList;
class QBoxLayout;
class QLabel;
class QPushButton;
class QSpacerItem;
class QVariantAnimation;
class QVBoxLayout;

// The left menu: brand block, workspace buttons, the saved chats under one of them, and Settings.
// It only asks (signals); the main window switches panels.
class Sidebar : public QWidget {
    Q_OBJECT

public:
    struct NavItem {
        QString label;
        QString icon;
        int panel;
    };

    // Workspaces in order, then the Settings item anchored at the bottom. historyPanel is the panel
    // whose button the chat list sits under. Ctrl+(panel+1) is a shortcut for every item.
    Sidebar(const QList<NavItem> &workspaces, const NavItem &settings, int historyPanel, QWidget *parent = nullptr);

    // Checks that panel's button and shows the chat list only while the history panel is open.
    void setCurrentPanel(int panel);
    // Re-tints the nav icons and the chat list's icons for the current theme.
    void refreshIcons();

    bool isCollapsed() const { return m_state.collapsed(); }
    // Applies the state (sliding when animate is true); does not store it. A change is announced.
    void setCollapsed(bool collapsed, bool animate = false);
    // The user's toggle (button or Ctrl+B): flips, slides unless motion is off, and stores the choice.
    void toggle();
    // Turns the chat list's New chat row and the + under Playground off together while a chat is busy.
    void setBusy(bool busy);
    // Verification runs turn the slide off; reduced motion in Settings does too.
    void setAnimationsEnabled(bool enabled) { m_animated = enabled; }

    ChatHistoryList *history() const { return m_history; }
    QWidget *brandSeal() const { return m_seal; }

signals:
    void panelRequested(int panel);
    void collapsedChanged(bool collapsed);
    void newChatRequested();

private:
    QPushButton *makeNavButton(const NavItem &item);
    void applyToggle();
    void applyPresentation();
    void updateExtras();
    void slideTo(int width, bool animate);

    SidebarState m_state;

    int m_historyPanel;
    int m_current = -1;
    QList<QPushButton *> m_buttons;
    ChatHistoryList *m_history = nullptr;
    QWidget *m_seal = nullptr;
    QPushButton *m_toggle = nullptr;
    QWidget *m_wordmark = nullptr;
    QList<QLabel *> m_tags;
    QVBoxLayout *m_navigation = nullptr;
    QVBoxLayout *m_layout = nullptr;
    QBoxLayout *m_top = nullptr;
    QSpacerItem *m_gap = nullptr;
    QSpacerItem *m_topGap = nullptr;
    QPushButton *m_newChat = nullptr;
    QPushButton *m_playground = nullptr;
    QVariantAnimation *m_slide = nullptr;
    bool m_animated = true;
    bool m_busy = false;
};
