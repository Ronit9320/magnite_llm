#include "widgets/sidebar.h"
#include "widgets/shortcuts.h"

#include "theme.h"
#include "widgets/brandmark.h"
#include "widgets/chathistorylist.h"
#include "widgets/sidebarstate.h"

#include <QBoxLayout>
#include <QLabel>
#include <QFrame>
#include <QPushButton>
#include <QShortcut>
#include <QSpacerItem>
#include <QSettings>
#include <QStyle>
#include <QVariantAnimation>
#include <QVBoxLayout>

Sidebar::Sidebar(const QList<NavItem> &workspaces, const NavItem &settings, int historyPanel, QWidget *parent)
    : QWidget(parent), m_state(SidebarState::load()), m_historyPanel(historyPanel) {
    setObjectName("sidebar");
    setAttribute(Qt::WA_StyledBackground, true);   // a QWidget subclass needs this for the QSS background
    setFixedWidth(SidebarState::kExpandedWidth);

    auto *layout = new QVBoxLayout(this);
    m_layout = layout;
    layout->setContentsMargins(14, 20, 14, 16);
    layout->setSpacing(6);
    auto *brand = new QVBoxLayout();
    layout->addLayout(brand);

    // The logo and the collapse button share one row when expanded and stack when collapsed.
    m_top = new QBoxLayout(QBoxLayout::LeftToRight);
    m_top->setSpacing(6);
    brand->addLayout(m_top);
    m_seal = new BrandSymbol(this);
    m_seal->setFixedSize(35, 35);
    m_seal->setToolTip("Magnite · Intelligence on your own terms");
    m_toggle = new QPushButton(this);
    m_toggle->setObjectName("sidebarToggle");
    m_toggle->setFixedSize(18, 24);
    m_toggle->setCursor(Qt::PointingHandCursor);
    connect(m_toggle, &QPushButton::clicked, this, &Sidebar::toggle);
    m_top->addWidget(m_seal);
    // Expanded, this pushes the toggle to the right edge; collapsed (stacked) it must not take height.
    m_topGap = new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Minimum);
    m_top->addSpacerItem(m_topGap);
    m_top->addWidget(m_toggle, 0, Qt::AlignTop);
    m_wordmark = new BrandWordmark(this);
    m_wordmark->setObjectName("logoLabel");
    m_wordmark->setFixedSize(86,24);
    m_top->insertWidget(1,m_wordmark);
    auto *edition = new QLabel("LOCAL STUDIO", this);
    edition->setObjectName("studioTag");
    edition->hide();

    m_gap = new QSpacerItem(0, 24, QSizePolicy::Minimum, QSizePolicy::Fixed);
    layout->addSpacerItem(m_gap);
    auto *workspaceLabel = new QLabel("WORKSPACE", this);
    workspaceLabel->setObjectName("studioTag");
    layout->addWidget(workspaceLabel);
    m_tags = {workspaceLabel};
    auto *navigation = new QVBoxLayout();
    navigation->setSpacing(5);
    layout->addLayout(navigation, 1);

    for (const NavItem &item : workspaces) {
        auto *button = makeNavButton(item);
        navigation->addWidget(button);
        if (item.panel == m_historyPanel) {
            m_playground = button;
            // Collapsed, a small + stands in for the chat list's New chat row.
            m_newChat = new QPushButton(this);
            m_newChat->setObjectName("sidebarNewChat");
            m_newChat->setFixedSize(26, 26);
            m_newChat->setToolTip("New chat");
            m_newChat->setAccessibleName("New chat");
            m_newChat->setCursor(Qt::PointingHandCursor);
            m_newChat->hide();
            connect(m_newChat, &QPushButton::clicked, this, &Sidebar::newChatRequested);
            navigation->addWidget(m_newChat, 0, Qt::AlignHCenter);
            // Saved chats nest under their workspace and only show while it is the open one.
            m_history = new ChatHistoryList(this);
            m_history->setContentsMargins(10, 0, 0, 0);
            m_history->hide();
            navigation->addWidget(m_history);
        }
    }
    if (!m_history) {
        m_newChat = new QPushButton(this);
        m_newChat->setObjectName("sidebarNewChat");
        m_newChat->setFixedSize(26, 26);
        m_newChat->setToolTip("New conversation");
        m_newChat->setAccessibleName("New conversation");
        connect(m_newChat, &QPushButton::clicked, this, &Sidebar::newChatRequested);
        navigation->addWidget(m_newChat, 0, Qt::AlignHCenter);
        m_history = new ChatHistoryList(this);
        m_history->useConversationWorkspace();
        navigation->addWidget(m_history, 1);
        navigation->addStretch();   // collapsed, the hidden list leaves its height here so the icons stay at the top
        auto *open = new QShortcut(Shortcuts::workspace(m_historyPanel), this);
        connect(open, &QShortcut::activated, this, [this] { emit panelRequested(m_historyPanel); });
    }
    if (m_playground) navigation->addStretch();
    auto *local=new QFrame(this); local->setObjectName("sidebarLocalCard");
    auto *localBody=new QVBoxLayout(local); localBody->setContentsMargins(12,12,12,12); localBody->setSpacing(6);
    auto *localName=new QLabel("● Local workspace",local); localName->setObjectName("sidebarLocalName"); localBody->addWidget(localName);
    auto *localDetail=new QLabel("Your machine. Your models.",local); localDetail->setObjectName("sidebarLocalDetail"); localDetail->setWordWrap(true); localBody->addWidget(localDetail);
    auto *footer = new QVBoxLayout;
    footer->setSpacing(6);
    footer->addWidget(local);
    footer->addWidget(makeNavButton(settings));
    layout->addLayout(footer);

    auto *collapseKey = new QShortcut(Shortcuts::collapseSidebar(), this);
    collapseKey->setContext(Qt::WindowShortcut);
    connect(collapseKey, &QShortcut::activated, this, &Sidebar::toggle);
    m_slide = new QVariantAnimation(this);
    m_slide->setDuration(180);
    m_slide->setEasingCurve(QEasingCurve::InOutCubic);
    connect(m_slide, &QVariantAnimation::valueChanged, this, [this](const QVariant &value) { setFixedWidth(value.toInt()); });
    connect(m_slide, &QVariantAnimation::finished, this, [this]() {
        applyPresentation();   // an expanding sidebar shows its text only once it is wide enough
        setFixedWidth(m_state.width());
    });
    applyToggle();
    applyPresentation();
    setFixedWidth(m_state.width());   // a stored collapsed choice shows from the very first paint
}

QPushButton *Sidebar::makeNavButton(const NavItem &item) {
    auto *button = new QPushButton(item.label);
    button->setObjectName("navBtn");
    button->setCheckable(true);
    button->setCursor(Qt::PointingHandCursor);
    button->setProperty("navicon", item.icon);
    button->setProperty("panel", item.panel);
    button->setProperty("label", item.label);
    connect(button, &QPushButton::clicked, this, [this, item]() { emit panelRequested(item.panel); });
    button->setToolTip(item.label + " · " + Shortcuts::label(Shortcuts::workspace(item.panel)));
    button->setMinimumHeight(40);
    auto *shortcut = new QShortcut(Shortcuts::workspace(item.panel), this);
    connect(shortcut, &QShortcut::activated, button, &QPushButton::click);
    m_buttons.append(button);
    return button;
}

void Sidebar::setCurrentPanel(int panel) {
    m_current = panel;
    for (auto *button : m_buttons)
        button->setChecked(button->property("panel").toInt() == panel);
    updateExtras();
    refreshIcons();
}

void Sidebar::setCollapsed(bool collapsed, bool animate) {
    if (m_state.collapsed() == collapsed) return;
    // Focus inside a list that is about to disappear moves to the Playground button.
    // (window()->focusWidget() is where focus really is; this widget's own focusWidget() can be a stale memory.)
    QWidget *focused = window()->focusWidget();
    if (collapsed && m_history && focused && m_history->isAncestorOf(focused))
        (m_playground ? m_playground : m_toggle)->setFocus();
    m_state.setCollapsed(collapsed);
    applyToggle();
    if (!animate) {
        m_slide->stop();
        applyPresentation();
        setFixedWidth(m_state.width());
    } else {
        if (collapsed) applyPresentation();   // a collapsing sidebar drops its text at the start of the slide
        slideTo(m_state.width(), true);
    }
    emit collapsedChanged(collapsed);
}

void Sidebar::toggle() {
    const bool reduced = QSettings().value("appearance/studio3ReducedMotion", false).toBool();
    setCollapsed(!m_state.collapsed(), m_animated && !reduced);
    m_state.save();
}

void Sidebar::setBusy(bool busy) {
    m_busy = busy;
    if (m_history) m_history->setBusy(busy);
    if (m_newChat) m_newChat->setEnabled(!busy);
    refreshIcons();
}

void Sidebar::slideTo(int width, bool animate) {
    m_slide->stop();
    if (!animate || width == this->width()) {
        setFixedWidth(width);
        applyPresentation();   // no slide will run, so nothing else will finish the change
        return;
    }
    m_slide->setStartValue(this->width());
    m_slide->setEndValue(width);
    m_slide->start();
}

void Sidebar::applyToggle() {
    m_toggle->setToolTip(m_state.toggleTooltip());
    m_toggle->setAccessibleName(m_state.collapsed() ? "Expand sidebar" : "Collapse sidebar");
    m_toggle->setIcon(Theme::coloredIcon(m_state.collapsed() ? "chevron-right" : "chevron-left", QColor(Theme::textSecondary()), 14));
    m_toggle->setIconSize(QSize(14, 14));
}

void Sidebar::updateExtras() {
    const bool playgroundOpen = m_current == m_historyPanel;
    if (m_history) m_history->setVisible(m_state.showHistory(playgroundOpen || !m_playground));
    if (m_newChat) m_newChat->setVisible(m_state.showNewChatButton(playgroundOpen || !m_playground));
}

void Sidebar::applyPresentation() {
    const bool collapsed = m_state.collapsed();
    setProperty("collapsed", collapsed);
    m_layout->setContentsMargins(collapsed ? 8 : 14, collapsed ? 14 : 20, collapsed ? 8 : 14, collapsed ? 12 : 16);
    m_gap->changeSize(0, collapsed ? 10 : 24, QSizePolicy::Minimum, QSizePolicy::Fixed);
    m_top->setDirection(collapsed ? QBoxLayout::TopToBottom : QBoxLayout::LeftToRight);
    if (collapsed) m_topGap->changeSize(0, 6, QSizePolicy::Minimum, QSizePolicy::Fixed);
    else m_topGap->changeSize(0, 0, QSizePolicy::Expanding, QSizePolicy::Minimum);
    m_top->setAlignment(m_seal, collapsed ? Qt::AlignHCenter : Qt::Alignment());
    m_top->setAlignment(m_toggle, collapsed ? Qt::AlignHCenter : Qt::AlignTop);
    const int side = collapsed ? 30 : 35;
    m_seal->setFixedSize(side, side);
    m_wordmark->setVisible(m_state.showLabels());
    findChild<QWidget *>("sidebarLocalCard")->setVisible(m_state.showLabels());
    for (auto *tag : m_tags) tag->setVisible(m_state.showLabels());
    for (auto *button : m_buttons) {
        const QString label = button->property("label").toString();
        button->setText(m_state.showLabels() ? label : QString());
        button->setAccessibleName(label);
        button->setMinimumHeight(40);
    }
    updateExtras();
    // Dynamic-property rules (QWidget#sidebar[collapsed="true"] ...) apply to the children too.
    style()->unpolish(this);
    style()->polish(this);
    for (auto *child : findChildren<QWidget *>()) {
        style()->unpolish(child);
        style()->polish(child);
    }
    m_layout->invalidate();
    refreshIcons();
}

void Sidebar::refreshIcons() {
    // Stroke-tinted icons: accent on the open workspace, secondary when idle.
    for (auto *button : m_buttons) {
        const QString name = button->property("navicon").toString();
        const QColor color = button->isChecked() ? QColor(Theme::accentPrimary()) : QColor(Theme::textSecondary());
        button->setIcon(Theme::coloredIcon(name, color, 18));
        button->setIconSize(QSize(18, 18));
    }
    applyToggle();
    if (m_newChat) {
        m_newChat->setIcon(Theme::coloredIcon("plus", QColor(m_newChat->isEnabled() ? Theme::accentLabel() : Theme::textMuted()), 14));
        m_newChat->setIconSize(QSize(14, 14));
    }
    if (m_history) m_history->refreshIcons();
}
