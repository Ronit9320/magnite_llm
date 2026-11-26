#include "panels/blueprintquickadd.h"

#include "panels/blueprintpreview.h"
#include "theme.h"
#include "widgets/blueprintstyle.h"

#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QVBoxLayout>

namespace {
constexpr int kWidth = 300;
constexpr int kMaxHeight = 360;
constexpr int kEntryRole = Qt::UserRole;
}  // namespace

BlueprintQuickAdd::BlueprintQuickAdd(BlueprintPreview &preview, QWidget *parent)
    : FloatingPanel(parent), m_preview(preview) {
    setObjectName("blueprintQuickAdd");
    setAccessibleName("Quick add");
    auto *body = new QVBoxLayout(this);
    body->setSpacing(6);
    m_search = new QLineEdit;
    m_search->setAccessibleName("Search nodes to add");
    m_search->setPlaceholderText("Search nodes");
    m_search->installEventFilter(this);
    body->addWidget(m_search);
    m_list = new QListWidget;
    m_list->setObjectName("blueprintQuickAddList");
    m_list->setAccessibleName("Nodes to add");
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setIconSize(QSize(15, 15));
    m_list->setFocusPolicy(Qt::NoFocus);
    body->addWidget(m_list, 1);
    connect(m_search, &QLineEdit::textChanged, this, &BlueprintQuickAdd::refresh);
    connect(m_list, &QListWidget::itemClicked, this, [this] { accept(false); });
    hide();
}

void BlueprintQuickAdd::open(const QPoint &position, Filter filter) {
    m_filter = std::move(filter);
    m_search->clear();
    refresh();
    auto *host = parentWidget();
    const int height = qMin(kMaxHeight, sizeHint().height() + m_list->sizeHintForRow(0) * m_list->count());
    resize(kWidth + 2 * shadowMargin(), height);
    QPoint at = position - QPoint(shadowMargin(), shadowMargin());
    if (host) {
        // Flip to stay inside the canvas.
        if (at.x() + width() > host->width()) at.rx() = position.x() - width() + shadowMargin();
        if (at.y() + height > host->height()) at.ry() = position.y() - height + shadowMargin();
        at.rx() = qBound(0, at.x(), qMax(0, host->width() - width()));
        at.ry() = qBound(0, at.y(), qMax(0, host->height() - height));
        host->installEventFilter(this);
    }
    QWidget::move(at);
    show();
    raise();
    m_search->setFocus();
}

void BlueprintQuickAdd::dismiss() {
    if (!isVisible()) return;
    if (auto *host = parentWidget()) host->removeEventFilter(this);
    hide();
    emit closed();
}

void BlueprintQuickAdd::refresh() {
    m_list->clear();
    for (const auto &[group, entries] : BlueprintLibraryDock::groups(m_preview, m_search->text().trimmed(), {})) {
        QList<BlueprintLibraryDock::Entry> shown;
        for (const auto &entry : entries)
            if (!m_filter || m_filter(entry)) shown.append(entry);
        if (shown.isEmpty()) continue;
        auto *header = new QListWidgetItem(group == BlueprintLibraryDock::savedGroup
            ? QString(BlueprintLibraryDock::savedGroup) : BlueprintStyle::familyLabel(group), m_list);
        header->setFlags(Qt::NoItemFlags);
        for (const auto &entry : shown) {
            const auto spec = m_preview.operation(entry.operation.isEmpty()
                ? BlueprintPreview::operationId(entry.savedNode) : entry.operation);
            const auto kind = spec["kind"].toString(entry.savedNode["kind"].toString());
            auto *item = new QListWidgetItem(Theme::coloredIcon(BlueprintStyle::nodeIcon(kind),
                BlueprintStyle::familyColor(BlueprintStyle::nodeFamily(kind)), 15),
                BlueprintLibraryDock::title(m_preview, entry), m_list);
            item->setData(kEntryRole, entry.toJson());
            item->setToolTip(spec["detail"].toString());
        }
    }
    moveSelection(1);
}

void BlueprintQuickAdd::moveSelection(int step) {
    if (!m_list->count()) return;
    int row = m_list->currentRow();
    if (step > 0 && row < 0) row = -1;
    for (int i = 0; i < m_list->count(); ++i) {
        row = (row + step + m_list->count()) % m_list->count();
        if (m_list->item(row)->flags() & Qt::ItemIsSelectable) {
            m_list->setCurrentRow(row);
            return;
        }
    }
}

void BlueprintQuickAdd::accept(bool details) {
    auto *item = m_list->currentItem();
    if (!item || !(item->flags() & Qt::ItemIsSelectable)) return;
    const auto entry = BlueprintLibraryDock::Entry::fromJson(item->data(kEntryRole).toByteArray());
    if (details) {
        emit detailsRequested(entry);
        return;
    }
    dismiss();
    emit accepted(entry);
}

bool BlueprintQuickAdd::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_search && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        switch (key->key()) {
        case Qt::Key_Down: moveSelection(1); return true;
        case Qt::Key_Up: moveSelection(-1); return true;
        case Qt::Key_Return:
        case Qt::Key_Enter: accept(key->modifiers() & Qt::ShiftModifier); return true;
        case Qt::Key_Escape: dismiss(); return true;
        default: break;
        }
    }
    if (watched == parentWidget() && event->type() == QEvent::MouseButtonPress
        && !geometry().contains(static_cast<QMouseEvent *>(event)->position().toPoint()))
        dismiss();
    return FloatingPanel::eventFilter(watched, event);
}
