#include "widgets/chathistorylist.h"

#include "theme.h"

#include <QAction>
#include <QKeyEvent>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QScrollBar>
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
    layout->addStretch(1);   // spare height stays below the list rather than spreading between the rows

    connect(m_new, &QPushButton::clicked, this, &ChatHistoryList::newChatRequested);
    connect(m_list, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
        requestOpen(item->data(Qt::UserRole).toString());
        highlightOpen();   // a refused or failed open must not leave another row looking opened
    });
    connect(m_list, &QListWidget::customContextMenuRequested, this, [this](const QPoint &pos) { showMenuAt(pos); });
    refreshIcons();
    updateHeights();
}

void ChatHistoryList::refreshIcons() {
    m_new->setIcon(Theme::coloredIcon("plus", QColor(m_new->isEnabled() ? Theme::accentLabel() : Theme::textMuted()), 14));
}

QString ChatHistoryList::idOf(int row) const {
    return row >= 0 && row < m_list->count() ? m_list->item(row)->data(Qt::UserRole).toString() : QString();
}

void ChatHistoryList::setChats(const QList<ChatSummary> &chats, const QString &openId) {
    m_openId = openId;
    const int scroll = m_list->verticalScrollBar()->value();
    m_list->clear();
    for (const ChatSummary &chat : chats) {
        auto *item = new QListWidgetItem(chat.title, m_list);
        item->setData(Qt::UserRole, chat.id);
        item->setToolTip(chat.title);
        item->setSizeHint(QSize(0, kRowHeight));
        if (chat.id == openId) m_list->setCurrentItem(item);
    }
    updateHeights();
    m_list->doItemsLayout();   // brings the scroll range up to date so the old position can be restored
    m_list->verticalScrollBar()->setValue(scroll);
}

void ChatHistoryList::updateHeights() {
    const int rows = m_list->count();
    m_list->setVisible(rows > 0);
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
        if (idOf(row) == m_openId) {
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
    const bool allowed = !(m_busy && id == m_openId);   // the open chat's own actions wait for the busy work to end
    QAction *rename = menu->addAction("Rename");
    rename->setEnabled(allowed);
    connect(rename, &QAction::triggered, this, [this, id]() { emit renameRequested(id); });
    QAction *remove = menu->addAction("Delete");
    remove->setEnabled(allowed);
    connect(remove, &QAction::triggered, this, [this, id]() { emit deleteRequested(id); });
    return menu;
}

void ChatHistoryList::showMenuAt(const QPoint &viewportPos) {
    // From the keyboard's menu key the position is not on a row: use the current row then.
    QListWidgetItem *item = m_list->itemAt(viewportPos);
    if (!item) item = m_list->currentItem();
    if (!item) return;
    QMenu *menu = contextMenuFor(item->data(Qt::UserRole).toString());
    menu->setAttribute(Qt::WA_DeleteOnClose);
    menu->popup(m_list->viewport()->mapToGlobal(viewportPos));
}

bool ChatHistoryList::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_list && event->type() == QEvent::KeyPress) {
        const auto *key = static_cast<QKeyEvent *>(event);
        const QString id = idOf(m_list->currentRow());
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
