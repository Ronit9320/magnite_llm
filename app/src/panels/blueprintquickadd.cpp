#include "panels/blueprintquickadd.h"

#include "panels/blueprintpreview.h"
#include "theme.h"
#include "widgets/blueprintstyle.h"

#include <QAbstractScrollArea>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QStyledItemDelegate>
#include <QVBoxLayout>

namespace {
constexpr int kWidth = 360;
constexpr int kMaxListHeight = 340;
constexpr int kGlyph = 28;
constexpr int kInfo = 22;

// Paints one quick-add row: a tinted glyph tile, the name over a one-line description, the input a
// dropped wire would connect to, and an ⓘ on the highlighted row. Group headers are small capitals.
class QuickAddDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    static QRect infoRect(const QRect &row) {
        return QRect(row.right() - kInfo - 6, row.center().y() - kInfo / 2, kInfo, kInfo);
    }

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        const QFontMetrics title(option.font);
        if (index.data(BlueprintQuickAdd::GroupRole).toBool()) return {kWidth, title.height() + 14};
        return {kWidth, qMax(kGlyph + 14, title.height() * 2 + 16)};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing);
        const QRect row = option.rect.adjusted(4, 1, -4, -1);
        QFont small = option.font;
        small.setPixelSize(qMax(11, qRound(option.font.pixelSize() * 12.0 / 13.0)));

        if (index.data(BlueprintQuickAdd::GroupRole).toBool()) {
            QFont group = small;
            group.setPixelSize(qMax(11, qRound(option.font.pixelSize() * 11.0 / 13.0)));
            group.setWeight(QFont::Medium);
            group.setCapitalization(QFont::AllUppercase);
            group.setLetterSpacing(QFont::AbsoluteSpacing, 0.8);
            painter->setFont(group);
            painter->setPen(QColor(Theme::textMuted()));
            painter->drawText(row.adjusted(8, 0, -8, -3), Qt::AlignLeft | Qt::AlignBottom, index.data().toString());
            painter->restore();
            return;
        }

        const bool current = option.state & QStyle::State_Selected;
        if (current) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(QColor(Theme::fieldFillHover()));
            painter->drawRoundedRect(row, 9, 9);
        }

        const QColor family = index.data(BlueprintQuickAdd::ColorRole).value<QColor>();
        const QRect glyph(row.left() + 8, row.center().y() - kGlyph / 2, kGlyph, kGlyph);
        QColor tint = family;
        tint.setAlphaF(0.16);
        painter->setPen(Qt::NoPen);
        painter->setBrush(tint);
        painter->drawRoundedRect(glyph, 8, 8);
        const auto icon = Theme::coloredIcon(index.data(BlueprintQuickAdd::IconRole).toString(), family, 15);
        icon.paint(painter, glyph.adjusted(6, 6, -6, -6));

        int right = row.right() - 8;
        if (current) right = infoRect(option.rect).left() - 6;
        if (current) {
            const auto info = Theme::coloredIcon("info", QColor(Theme::textSecondary()), 14);
            info.paint(painter, infoRect(option.rect).adjusted(4, 4, -4, -4));
        }
        const QString accepts = index.data(BlueprintQuickAdd::AcceptsRole).toString();
        if (!accepts.isEmpty()) {
            QFont mono(Theme::monoFont());
            mono.setPixelSize(qMax(11, small.pixelSize() - 1));
            const QFontMetrics metrics(mono);
            const QString text = metrics.elidedText(QStringLiteral("→ ") + accepts, Qt::ElideRight, 110);
            const int width = metrics.horizontalAdvance(text);
            painter->setFont(mono);
            painter->setPen(QColor(Theme::textMuted()));
            painter->drawText(QRect(right - width, row.top(), width, row.height()), Qt::AlignVCenter | Qt::AlignRight, text);
            right -= width + 10;
        }

        const int left = glyph.right() + 11;
        const QFontMetrics titleMetrics(option.font), detailMetrics(small);
        const int textHeight = titleMetrics.height() + detailMetrics.height();
        const int top = row.center().y() - textHeight / 2;
        QFont title = option.font;
        title.setWeight(QFont::Medium);
        painter->setFont(title);
        painter->setPen(QColor(Theme::textPrimary()));
        painter->drawText(QRect(left, top, right - left, titleMetrics.height()), Qt::AlignLeft | Qt::AlignVCenter,
                          QFontMetrics(title).elidedText(index.data().toString(), Qt::ElideRight, right - left));
        painter->setFont(small);
        painter->setPen(QColor(Theme::textMuted()));
        painter->drawText(QRect(left, top + titleMetrics.height(), right - left, detailMetrics.height()),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          detailMetrics.elidedText(index.data(BlueprintQuickAdd::DetailRole).toString(), Qt::ElideRight, right - left));
        painter->restore();
    }
};

QLabel *hint(QHBoxLayout *layout, const QStringList &keys, const QString &what) {
    for (const auto &key : keys) {
        auto *chip = new QLabel(key);
        chip->setObjectName("blueprintKbd");
        chip->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
        layout->addWidget(chip);
    }
    auto *label = new QLabel(what);
    label->setObjectName("blueprintQuickAddHint");
    label->setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    layout->addWidget(label);
    layout->addSpacing(6);
    return label;
}
}  // namespace

BlueprintQuickAdd::BlueprintQuickAdd(BlueprintPreview &preview, QWidget *parent)
    : FloatingPanel(parent), m_preview(preview) {
    setObjectName("blueprintQuickAdd");
    setAccessibleName("Quick add");
    auto *body = new QVBoxLayout(this);
    body->setSpacing(0);

    m_search = new QLineEdit;
    m_search->setObjectName("blueprintQuickAddSearch");
    m_search->setAccessibleName("Search nodes to add");
    m_search->setPlaceholderText("Search nodes");
    m_search->addAction(Theme::coloredIcon("search", QColor(Theme::textMuted()), 16), QLineEdit::LeadingPosition);
    m_search->installEventFilter(this);
    auto *searchRow = new QVBoxLayout;
    searchRow->setContentsMargins(4, 4, 4, 4);
    searchRow->addWidget(m_search);
    body->addLayout(searchRow);

    m_context = new QWidget;
    auto *context = new QHBoxLayout(m_context);
    context->setContentsMargins(12, 2, 12, 4);
    context->setSpacing(6);
    m_contextDot = new QLabel;
    m_contextDot->setFixedSize(8, 8);
    context->addWidget(m_contextDot);
    m_contextText = new QLabel;
    m_contextText->setObjectName("blueprintQuickAddHint");
    m_contextText->setTextFormat(Qt::PlainText);
    context->addWidget(m_contextText, 1);
    m_context->hide();
    body->addWidget(m_context);

    m_list = new QListWidget;
    m_list->setObjectName("blueprintQuickAddList");
    m_list->setAccessibleName("Nodes to add");
    m_list->setFrameShape(QFrame::NoFrame);
    m_list->setFocusPolicy(Qt::NoFocus);
    m_list->setMouseTracking(true);
    m_list->setUniformItemSizes(false);
    m_list->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    m_list->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_list->setItemDelegate(new QuickAddDelegate(m_list));
    m_list->viewport()->installEventFilter(this);
    body->addWidget(m_list, 1);

    auto *foot = new QFrame;
    foot->setObjectName("blueprintQuickAddFoot");
    auto *keys = new QHBoxLayout(foot);
    keys->setContentsMargins(10, 7, 10, 7);
    keys->setSpacing(4);
    hint(keys, {"↑↓"}, "move");
    hint(keys, {"Enter"}, "add");
    hint(keys, {"⇧ Enter"}, "details");
    hint(keys, {"Esc"}, "close");
    keys->addStretch();
    body->addWidget(foot);

    connect(m_search, &QLineEdit::textChanged, this, &BlueprintQuickAdd::refresh);
    hide();
}

void BlueprintQuickAdd::open(const QPoint &position, Filter filter) {
    m_filter = std::move(filter);
    m_accepts = {};
    m_context->hide();
    place(position);
}

void BlueprintQuickAdd::openForWire(const QPoint &position, const QString &typeLabel, const QColor &typeColor,
                                    Accepts accepts) {
    m_accepts = std::move(accepts);
    m_filter = [this](const BlueprintLibraryDock::Entry &entry) { return !m_accepts(entry).isEmpty(); };
    m_contextDot->setStyleSheet(QString("background: %1; border-radius: 4px;").arg(typeColor.name()));
    m_contextText->setText(QString("Nodes that accept %1 · connects on add").arg(typeLabel.toLower()));
    m_context->show();
    place(position);
}

void BlueprintQuickAdd::place(const QPoint &position) {
    m_search->clear();
    refresh();
    auto *host = parentWidget();
    int rows = 0;
    for (int i = 0; i < m_list->count(); ++i) rows += m_list->sizeHintForRow(i);
    m_list->setFixedHeight(qBound(m_list->sizeHintForRow(0) * 2, rows + 6, kMaxListHeight));
    layout()->activate();
    // The key hints set the floor at larger interface sizes.
    resize(qMax(kWidth + 2 * shadowMargin(), sizeHint().width()), sizeHint().height());
    const int height = this->height();
    QPoint at = position - QPoint(shadowMargin(), shadowMargin());
    if (host) {
        // Flip, then clamp, to stay inside the open canvas; the shadow may spill past it.
        const QRect bounds = (m_bounds.isEmpty() ? host->rect() : m_bounds)
            .adjusted(-shadowMargin(), -shadowMargin(), shadowMargin(), shadowMargin());
        if (at.x() + width() > bounds.right()) at.rx() = position.x() - width() + shadowMargin();
        if (at.y() + height > bounds.bottom()) at.ry() = position.y() - height + shadowMargin();
        at.rx() = qBound(bounds.left(), at.x(), qMax(bounds.left(), bounds.right() + 1 - width()));
        at.ry() = qBound(bounds.top(), at.y(), qMax(bounds.top(), bounds.bottom() + 1 - height));
        host->installEventFilter(this);
        if (auto *area = qobject_cast<QAbstractScrollArea *>(host)) area->viewport()->installEventFilter(this);
    }
    QWidget::move(at);
    QWidget::show();
    raise();
    m_search->setFocus();
}

void BlueprintQuickAdd::dismiss() {
    if (!isVisible()) return;
    if (auto *host = parentWidget()) {
        host->removeEventFilter(this);
        if (auto *area = qobject_cast<QAbstractScrollArea *>(host)) area->viewport()->removeEventFilter(this);
    }
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
        header->setData(GroupRole, true);
        for (const auto &entry : shown) {
            const auto spec = m_preview.operation(entry.operation.isEmpty()
                ? BlueprintPreview::operationId(entry.savedNode) : entry.operation);
            const auto kind = spec["kind"].toString(entry.savedNode["kind"].toString());
            auto *item = new QListWidgetItem(BlueprintLibraryDock::title(m_preview, entry), m_list);
            item->setData(EntryRole, entry.toJson());
            item->setData(DetailRole, entry.operation.isEmpty() ? spec["label"].toString() : spec["detail"].toString());
            item->setData(ColorRole, BlueprintStyle::familyColor(BlueprintStyle::nodeFamily(kind)));
            item->setData(IconRole, BlueprintStyle::nodeIcon(kind));
            if (m_accepts) item->setData(AcceptsRole, m_accepts(entry));
            item->setToolTip(spec["detail"].toString());
        }
    }
    if (!m_list->count()) {
        auto *none = new QListWidgetItem("No nodes match", m_list);
        none->setFlags(Qt::NoItemFlags);
        none->setData(GroupRole, true);
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
            m_list->scrollToItem(m_list->item(row));
            return;
        }
    }
}

void BlueprintQuickAdd::accept(bool details) {
    auto *item = m_list->currentItem();
    if (!item || !(item->flags() & Qt::ItemIsSelectable)) return;
    const auto entry = BlueprintLibraryDock::Entry::fromJson(item->data(EntryRole).toByteArray());
    if (details) {
        emit detailsRequested(entry);
        return;
    }
    dismiss();
    emit accepted(entry);
}

bool BlueprintQuickAdd::eventFilter(QObject *watched, QEvent *event) {
    if (m_search && watched == m_search && event->type() == QEvent::KeyPress) {
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
    if (m_list && watched == m_list->viewport()) {
        if (event->type() == QEvent::MouseMove) {
            // The highlight follows the pointer, as it does the arrow keys.
            auto *item = m_list->itemAt(static_cast<QMouseEvent *>(event)->position().toPoint());
            if (item && (item->flags() & Qt::ItemIsSelectable) && item != m_list->currentItem()) m_list->setCurrentItem(item);
        } else if (event->type() == QEvent::MouseButtonPress) {
            const auto point = static_cast<QMouseEvent *>(event)->position().toPoint();
            auto *item = m_list->itemAt(point);
            if (item && (item->flags() & Qt::ItemIsSelectable)) {
                m_list->setCurrentItem(item);
                accept(QuickAddDelegate::infoRect(m_list->visualItemRect(item)).contains(point));
            }
            return true;
        }
    }
    // A press anywhere else on the canvas closes the menu.
    if (event->type() == QEvent::MouseButtonPress && parentWidget()) {
        auto *area = qobject_cast<QAbstractScrollArea *>(parentWidget());
        if (watched == parentWidget() || (area && watched == area->viewport())) {
            const auto global = static_cast<QMouseEvent *>(event)->globalPosition().toPoint();
            if (!geometry().contains(parentWidget()->mapFromGlobal(global))) dismiss();
        }
    }
    return FloatingPanel::eventFilter(watched, event);
}
