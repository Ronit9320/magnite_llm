#include "widgets/chathistorylist.h"

#include "theme.h"
#include "widgets/button.h"

#include <QAction>
#include <algorithm>
#include <QKeyEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMap>
#include <QListWidget>
#include <QMenu>
#include <QPixmap>
#include <QPushButton>
#include <QScrollBar>
#include <QSettings>
#include <QVBoxLayout>

ChatHistoryList::ChatHistoryList(QWidget *parent)
    : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);

    m_new = new QPushButton("New chat", this);
    m_new->setObjectName("historyNew");
    m_new->setAccessibleName("New chat");
    m_new->setCursor(Qt::PointingHandCursor);
    m_new->setMinimumHeight(kRowHeight);
    layout->addWidget(m_new);

    m_list = new QListWidget(this);
    m_list->setObjectName("historyList");
    m_list->setAccessibleName("Saved chats");
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_list->setTextElideMode(Qt::ElideRight);
    m_list->setSelectionMode(QAbstractItemView::SingleSelection);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setContextMenuPolicy(Qt::CustomContextMenu);
    m_list->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);
    m_list->installEventFilter(this);
    layout->addWidget(m_list);
    m_notice = new QLabel(this);
    m_notice->setObjectName("historyNotice");
    m_notice->setWordWrap(true);
    m_notice->setToolTip("Damaged chat files are left untouched in the chats folder.");
    m_notice->hide();
    layout->addWidget(m_notice);
    const auto saved = QSettings().value("sidebar/collapsedSections").toStringList();
    m_collapsed = QSet<QString>(saved.cbegin(), saved.cend());
    layout->addStretch(1);   // spare height stays below the list rather than spreading between the rows

    connect(m_new, &QPushButton::clicked, this, &ChatHistoryList::newChatRequested);
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        const QString key = item->data(kSectionKeyRole).toString();
        if (!key.isEmpty()) {
            toggleSection(key);
            return;
        }
        requestOpen(item->data(Qt::UserRole).toString());
        highlightOpen();   // a refused or failed open must not leave another row looking opened
    });
    connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) { showMenuAt(pos); });
    refreshIcons();
    updateHeights();
}

void ChatHistoryList::refreshIcons() {
    if (m_searchToggle)
        m_searchToggle->setIcon(Theme::coloredIcon("search", QColor(Theme::textSecondary()), 16));
    m_new->setIcon(Theme::coloredIcon("plus", QColor(m_new->isEnabled() ? Theme::accentLabel() : Theme::textMuted()), 14));
    for (int row = 0; m_list && row < m_list->count(); ++row) {
        auto *item = m_list->item(row);
        const QString key = item->data(kSectionKeyRole).toString();
        if (!key.isEmpty()) item->setIcon(sectionIcon(key, key.startsWith("project:")));
    }
}

QIcon ChatHistoryList::sectionIcon(const QString &key, bool project) const {
    if (project) return Theme::coloredIcon("folder", QColor(Theme::textSecondary()), 14);
    return Theme::coloredIcon(m_collapsed.contains(key) ? "chevron-right" : "chevron-down", QColor(Theme::textSecondary()), 14);
}

bool ChatHistoryList::isSection(int row) const {
    return row >= 0 && row < m_list->count() && !m_list->item(row)->data(kSectionKeyRole).toString().isEmpty();
}

void ChatHistoryList::toggleSection(const QString &key) {
    if (!m_collapsed.remove(key)) m_collapsed.insert(key);
    QSettings().setValue("sidebar/collapsedSections", QStringList(m_collapsed.cbegin(), m_collapsed.cend()));
    rebuild();
}

QString ChatHistoryList::idOf(int row) const {
    return row >= 0 && row < m_list->count() ? m_list->item(row)->data(Qt::UserRole).toString() : QString();
}

void ChatHistoryList::setChats(const QList<ChatSummary> &chats, const QString &openId) {
    m_openId = openId;
    m_chats = chats;
    rebuild();
}

void ChatHistoryList::useConversationWorkspace() {
    auto *box = qobject_cast<QVBoxLayout *>(layout());
    auto *heading = new QLabel("Conversations", this);
    heading->setObjectName("hubSectionTitle");
    auto *header = new QHBoxLayout;
    header->addWidget(heading, 1);
    m_searchToggle = new Button(this);
    m_searchToggle->setObjectName("conversationSearchToggle");
    m_searchToggle->setFlat(true);
    m_searchToggle->setCheckable(true);
    m_searchToggle->setFixedSize(30, 30);
    m_searchToggle->setIconSize(QSize(16, 16));
    m_searchToggle->setAccessibleName("Search conversations");
    m_searchToggle->setToolTip("Search conversations");
    header->addWidget(m_searchToggle);
    box->insertLayout(0, header);
    m_new->setProperty("conversationAction", true);
    m_new->setText("New conversation");
    m_new->setAccessibleName("New conversation");
    m_search = new QLineEdit(this);
    m_search->setPlaceholderText("Search conversations");
    m_search->setAccessibleName("Search conversations");
    m_search->setClearButtonEnabled(true);
    m_search->hide();
    m_search->installEventFilter(this);
    box->insertWidget(1, m_search);
    connect(m_searchToggle, &QPushButton::toggled, this, [this](bool open) {
        m_search->setVisible(open);
        if (open) {
            m_search->setFocus();
        } else {
            m_search->clear();
            m_searchToggle->setFocus();
        }
    });
    refreshIcons();
    box->setStretchFactor(m_list, 1);
    connect(m_search, &QLineEdit::textChanged, this, &ChatHistoryList::rebuild);
    updateHeights();
}

void ChatHistoryList::setProjectLookup(std::function<QStringList(const QString &)> lookup) {
    m_projectLookup = std::move(lookup);
    rebuild();
}

void ChatHistoryList::rebuild() {
    const int scroll = m_list->verticalScrollBar()->value();
    m_list->clear();
    const auto addChat = [this](const ChatSummary &chat, bool nested = false) {
        auto *item = new QListWidgetItem(chat.title, m_list);
        item->setData(Qt::UserRole, chat.id);
        item->setToolTip(chat.title);
        item->setSizeHint(QSize(0, kRowHeight));
        if (nested) {   // a blank icon indents the title under its project heading
            QPixmap blank(14, 14);
            blank.fill(Qt::transparent);
            item->setIcon(QIcon(blank));
        }
    };
    // A heading row folds and unfolds its group when clicked (or on Enter / Space from the keyboard).
    const auto addHeading = [this](const QString &key, const QString &title, bool project) {
        auto *item = new QListWidgetItem(title, m_list);
        item->setData(kSectionKeyRole, key);
        item->setIcon(sectionIcon(key, project));
        item->setForeground(QColor(project ? Theme::textPrimary() : Theme::textSecondary()));
        auto font = m_list->font();
        font.setBold(!project);
        item->setFont(font);
        item->setToolTip(title);
        item->setSizeHint(QSize(0, kRowHeight));
    };
    const QString query = m_search ? m_search->text().trimmed() : QString();
    const bool sections = m_search != nullptr;   // the Conversations workspace; the older flat list has no headings
    // While searching, every group opens so a match is never hidden inside a folded one.
    const auto open = [this, &query](const QString &key) { return !query.isEmpty() || !m_collapsed.contains(key); };
    QList<ChatSummary> pinned, other;
    QMap<QString, QList<ChatSummary>> projects;
    for (const auto &chat : m_chats) {
        const auto names = m_projectLookup ? m_projectLookup(chat.id) : QStringList();
        const bool titleMatches = chat.title.contains(query, Qt::CaseInsensitive);
        // A pinned chat is a shortcut: it also stays under its project, and a project's chats never go to Recents.
        if (sections && chat.pinned && titleMatches) pinned.append(chat);
        // A pinned chat outside any project lives under Pinned only, so it is never listed twice.
        if (names.isEmpty() && titleMatches && !(sections && chat.pinned)) other.append(chat);
        for (const auto &name : names) {
            if (titleMatches || name.contains(query, Qt::CaseInsensitive)) projects[name].append(chat);
        }
    }
    if (!sections) {
        for (auto it = projects.cbegin(); it != projects.cend(); ++it) {
            auto *item = new QListWidgetItem(it.key(), m_list);
            item->setFlags(Qt::NoItemFlags);
            item->setForeground(QColor(Theme::textSecondary()));
            auto font = m_list->font();
            font.setWeight(QFont::Medium);   // DM Sans ships Regular and Medium only
            item->setFont(font);
            item->setSizeHint(QSize(0, kRowHeight));
            for (const auto &chat : it.value()) addChat(chat);
        }
        if (!projects.isEmpty() && !other.isEmpty()) {
            auto *item = new QListWidgetItem("Other conversations", m_list);
            item->setFlags(Qt::NoItemFlags);
            item->setForeground(QColor(Theme::textSecondary()));
            auto font = m_list->font();
            font.setWeight(QFont::Medium);   // DM Sans ships Regular and Medium only
            item->setFont(font);
            item->setSizeHint(QSize(0, kRowHeight));
        }
        for (const auto &chat : other) addChat(chat);
    } else {
        if (!pinned.isEmpty()) {
            addHeading("pinned", "Pinned", false);
            if (open("pinned"))
                for (const auto &chat : pinned) addChat(chat);
        }
        if (!projects.isEmpty()) {
            addHeading("projects", "Projects", false);
            if (open("projects")) {
                for (auto it = projects.cbegin(); it != projects.cend(); ++it) {
                    const QString key = "project:" + it.key();
                    addHeading(key, it.key(), true);
                    if (open(key))
                        for (const auto &chat : it.value()) addChat(chat, true);
                }
            }
        }
        if (!other.isEmpty()) {
            addHeading("recents", "Recents", false);
            if (open("recents"))
                for (const auto &chat : other) addChat(chat);
        }
    }
    highlightOpen();
    updateHeights();
    m_list->doItemsLayout();   // brings the scroll range up to date so the old position can be restored
    m_list->verticalScrollBar()->setValue(scroll);
}

void ChatHistoryList::setUnreadableCount(int count) {
    m_notice->setText(count == 1 ? QStringLiteral("1 chat could not be read")
                                 : QStringLiteral("%1 chats could not be read").arg(count));
    m_notice->setVisible(count > 0);
}

void ChatHistoryList::updateHeights() {
    const int rows = m_list->count();
    m_list->setVisible(rows > 0);
    if (m_search) {
        auto *box = qobject_cast<QVBoxLayout *>(layout());
        box->setStretch(box->count() - 1, rows > 0 ? 0 : 1);
        m_list->setMaximumHeight(QWIDGETSIZE_MAX);
        m_list->setMinimumHeight(0);
        return;
    }
    // Up to five rows; in a short window it gives way down to two rows rather than growing the window.
    m_list->setMaximumHeight(qMin(rows, kVisibleRows) * kRowHeight);
    m_list->setMinimumHeight(qMin(rows, 2) * kRowHeight);
}

void ChatHistoryList::setBusy(bool busy) {
    if (m_busy == busy) return;
    m_busy = busy;
    m_new->setEnabled(!busy);
    refreshIcons();
}

void ChatHistoryList::highlightOpen() {
    for (int row = 0; row < m_list->count(); ++row) {
        if (!m_openId.isEmpty() && idOf(row) == m_openId) {
            m_list->setCurrentRow(row);
            return;
        }
    }
    m_list->setCurrentRow(-1);
}

void ChatHistoryList::requestOpen(const QString &id) {
    if (!id.isEmpty() && !m_busy) emit openRequested(id);
}

void ChatHistoryList::requestDelete(const QString &id) {
    if (id.isEmpty() || (m_busy && id == m_openId)) return;
    emit deleteRequested(id);
}

QMenu *ChatHistoryList::contextMenuFor(const QString &id) {
    auto *menu = new QMenu(this);
    menu->setObjectName("conversationOptions");
    menu->setWindowFlag(Qt::FramelessWindowHint);
    menu->setAttribute(Qt::WA_TranslucentBackground);
    menu->setAccessibleName("Conversation options");
    const bool allowed = !(m_busy && id == m_openId);   // the open chat's own actions wait for the busy work to end
    const bool isPinned = std::any_of(m_chats.cbegin(), m_chats.cend(), [&id](const ChatSummary &c) { return c.id == id && c.pinned; });
    QAction *pin = menu->addAction(Theme::coloredIcon(isPinned ? "pin-off" : "pin", QColor(Theme::textSecondary()), 16), isPinned ? "Unpin" : "Pin");
    pin->setEnabled(allowed);
    connect(pin, &QAction::triggered, this, [this, id, isPinned]() { emit pinRequested(id, !isPinned); });
    QAction *rename = menu->addAction(Theme::coloredIcon("pencil", QColor(Theme::textSecondary()), 16), "Rename");
    rename->setEnabled(allowed);
    connect(rename, &QAction::triggered, this, [this, id]() { emit renameRequested(id); });
    QAction *remove = menu->addAction(Theme::coloredIcon("trash", QColor(Theme::statusError()), 16), "Delete");
    remove->setEnabled(allowed);
    connect(remove, &QAction::triggered, this, [this, id]() { emit deleteRequested(id); });
    return menu;
}

void ChatHistoryList::showMenuAt(const QPoint &viewportPos) {
    // From the keyboard's menu key the position is not on a row: use the current row then.
    QListWidgetItem *item = m_list->itemAt(viewportPos);
    if (!item) item = m_list->currentItem();
    if (!item || item->data(Qt::UserRole).toString().isEmpty()) return;
    QMenu *menu = contextMenuFor(item->data(Qt::UserRole).toString());
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(m_list->viewport()->mapToGlobal(viewportPos));
}

bool ChatHistoryList::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_search && event->type() == QEvent::KeyPress &&
        static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape) {
        m_searchToggle->setChecked(false);
        return true;
    }
    if (watched == m_list && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        const QString id = idOf(m_list->currentRow());
        if (isSection(m_list->currentRow()) &&
            (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter || key->key() == Qt::Key_Space)) {
            toggleSection(m_list->currentItem()->data(kSectionKeyRole).toString());
            return true;
        }
        if (key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) {
            requestOpen(id);
            return true;
        }
        if (key->key() == Qt::Key_Delete) {
            requestDelete(id);
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
