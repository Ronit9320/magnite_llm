#include "panels/messagerow.h"

#include "theme.h"
#include "widgets/versionswitcher.h"

#include <QApplication>
#include <QEnterEvent>
#include <QFocusEvent>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QPushButton>
#include "widgets/button.h"
#include <QTextCursor>
#include <QTextDocument>
#include <QTimer>
#include <QVBoxLayout>

MessageRow::MessageRow(ChatBubble::Role role, ChatTree::NodeId node, const QString &text, QWidget *parent)
    : QWidget(parent)
    , m_role(role)
    , m_lineHost(nullptr)
    , m_bubble(nullptr)
    , m_editButton(nullptr)
    , m_retryButton(nullptr)
    , m_editor(nullptr)
    , m_editBox(nullptr)
    , m_switcher(nullptr)
    , m_node(node)
{
    setObjectName(QStringLiteral("messageRow"));
    auto *column = new QVBoxLayout(this);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(4);

    // The bubble line: [stretch] [pencil] [bubble] for the user, [bubble] [stretch] for the model.
    m_lineHost = new QWidget(this);
    auto *line = new QHBoxLayout(m_lineHost);
    line->setContentsMargins(0, 0, 0, 0);
    line->setSpacing(6);
    m_bubble = new ChatBubble(role, text, m_lineHost);
    m_bubble->setWidthReference(parent);   // size against the chat area, as before rows existed
    if (role == ChatBubble::User) {
        m_editButton = new Button(m_lineHost);
        m_editButton->setObjectName(QStringLiteral("editButton"));
        m_editButton->setIcon(Theme::coloredIcon(QStringLiteral("pencil"), QColor(Theme::textMuted()), 14));
        m_editButton->setFixedSize(24, 24);
        m_editButton->setFlat(true);
        m_editButton->setCursor(Qt::PointingHandCursor);
        m_editButton->setAccessibleName(QStringLiteral("Edit message"));
        m_editButton->setToolTip(QStringLiteral("Edit message"));
        m_editButton->hide();
        m_editButton->installEventFilter(this);
        line->addStretch(1);
        line->addWidget(m_editButton, 0, Qt::AlignTop);
        line->addWidget(m_bubble);
        connect(m_editButton, &QPushButton::clicked, this, [this]() { emit editRequested(m_node); });
    } else {
        m_retryButton = new Button(m_lineHost);
        m_retryButton->setObjectName(QStringLiteral("retryButton"));
        m_retryButton->setIcon(Theme::coloredIcon(QStringLiteral("refresh-cw"), QColor(Theme::textMuted()), 14));
        m_retryButton->setFixedSize(24, 24);
        m_retryButton->setFlat(true);
        m_retryButton->setCursor(Qt::PointingHandCursor);
        m_retryButton->setAccessibleName(QStringLiteral("Retry reply"));
        m_retryButton->setToolTip(QStringLiteral("Retry reply"));
        m_retryButton->setEnabled(false);
        m_retryButton->hide();
        m_retryButton->installEventFilter(this);
        line->addWidget(m_bubble);
        line->addWidget(m_retryButton, 0, Qt::AlignBottom);
        line->addStretch(1);
        connect(m_retryButton, &QPushButton::clicked, this, [this]() { emit retryClicked(m_node); });
    }
    column->addWidget(m_lineHost);

    if (role == ChatBubble::User) {
        // The inline editor, right-aligned and as wide as a bubble can be.
        m_editor = new QWidget(this);
        m_editor->setObjectName(QStringLiteral("messageEditor"));
        m_editor->setMaximumWidth(680);
        auto *editorColumn = new QVBoxLayout(m_editor);
        editorColumn->setContentsMargins(0, 0, 0, 0);
        editorColumn->setSpacing(8);
        m_editBox = new QPlainTextEdit(m_editor);
        m_editBox->setObjectName(QStringLiteral("messageEditBox"));
        m_editBox->setAccessibleName(QStringLiteral("Edit your message"));
        m_editBox->installEventFilter(this);
        editorColumn->addWidget(m_editBox);
        auto *buttons = new QHBoxLayout;
        buttons->setSpacing(8);
        buttons->addStretch(1);
        auto *cancel = new Button(QStringLiteral("Cancel"), m_editor);
        cancel->setObjectName(QStringLiteral("editCancel"));
        auto *save = new Button(QStringLiteral("Save && send"), m_editor);   // "&&" is a literal ampersand in a button label
        save->setObjectName(QStringLiteral("editSave"));
        buttons->addWidget(cancel);
        buttons->addWidget(save);
        editorColumn->addLayout(buttons);
        m_editor->hide();
        auto *editLine = new QHBoxLayout;
        editLine->setContentsMargins(0, 0, 0, 0);
        editLine->addStretch(1);
        editLine->addWidget(m_editor, 100);
        column->addLayout(editLine);
        connect(cancel, &QPushButton::clicked, this, [this]() { userCancel(); });
        connect(save, &QPushButton::clicked, this, [this]() { userSave(); });
        connect(m_editBox, &QPlainTextEdit::textChanged, this, [this]() { resizeEditor(); });
    }

    m_switcher = new VersionSwitcher(this);
    m_switcher->hide();
    column->addWidget(m_switcher, 0, role == ChatBubble::User ? Qt::AlignRight : Qt::AlignLeft);
    connect(m_switcher, &VersionSwitcher::step, this, [this](int delta) { emit versionStep(m_node, delta); });
    updateFocusPolicy();
}

void MessageRow::setNode(ChatTree::NodeId node) {
    m_node = node;
    updateEditButton();
    updateRetryButton();
}

void MessageRow::setVersion(int index, int total) { m_switcher->setVersion(index, total); }

void MessageRow::setActionsEnabled(bool enabled) {
    m_actionsEnabled = enabled;
    m_switcher->setEnabledForUse(enabled);
    if (m_editButton) m_editButton->setEnabled(enabled);
    updateEditButton();
    updateRetryButton();
}

void MessageRow::setRetryAvailable(bool available) {
    m_retryAvailable = available;
    updateRetryButton();
}

void MessageRow::setSwitcherEnabled(bool enabled) { m_switcher->setEnabledForUse(enabled); }

void MessageRow::beginEdit(const QString &text) {
    if (!m_editor) return;
    m_editing = true;
    m_editBox->setPlainText(text);
    m_editBox->moveCursor(QTextCursor::End);
    m_lineHost->hide();
    m_editor->show();
    resizeEditor();
    updateEditButton();
    m_editBox->setFocus();
}

void MessageRow::cancelEdit() {
    if (m_editing) closeEditor();
}

void MessageRow::closeEditor() {
    m_editing = false;
    m_editor->hide();
    m_lineHost->show();
    updateEditButton();
}

void MessageRow::userCancel() {
    closeEditor();
    emit editCancelled(m_node);
}

void MessageRow::userSave() {
    const QString text = m_editBox->toPlainText().trimmed();
    if (text.isEmpty() || (text == m_bubble->text() && !m_resendable)) {   // nothing to send: same as Cancel
        userCancel();
        return;
    }
    closeEditor();
    emit editSaved(m_node, text);
}

void MessageRow::resizeEditor() {
    const int lines = qMax(1, int(m_editBox->document()->size().height()));
    m_editBox->setFixedHeight(qBound(72, lines * m_editBox->fontMetrics().lineSpacing() + 26, 260));
}

void MessageRow::updateEditButton() {
    if (m_editButton) m_editButton->setVisible((m_hover || m_focusWithin) && !m_editing && m_actionsEnabled && m_node != ChatTree::kRoot);
    updateFocusPolicy();
}

void MessageRow::updateFocusPolicy() {
    const bool hasAction = m_actionsEnabled && m_node != ChatTree::kRoot && !m_editing &&
                           (m_role == ChatBubble::User || m_retryAvailable);
    setFocusPolicy(hasAction ? Qt::TabFocus : Qt::NoFocus);
}

void MessageRow::updateFocusWithin() {
    QWidget *focused = QApplication::focusWidget();
    m_focusWithin = focused && (focused == this || isAncestorOf(focused));
    updateEditButton();
    updateRetryButton();
}

void MessageRow::updateRetryButton() {
    if (!m_retryButton) return;
    const bool usable = m_retryAvailable && m_actionsEnabled && m_node != ChatTree::kRoot;
    m_retryButton->setEnabled(usable);
    m_retryButton->setVisible(usable && (m_hover || m_focusWithin));
    updateFocusPolicy();
}

void MessageRow::focusInEvent(QFocusEvent *event) {
    m_focusWithin = true;
    updateEditButton();
    updateRetryButton();
    QWidget::focusInEvent(event);
}

void MessageRow::focusOutEvent(QFocusEvent *event) {
    // Focus may be moving to this row's own button; decide once the move has finished.
    QTimer::singleShot(0, this, [this]() { updateFocusWithin(); });
    QWidget::focusOutEvent(event);
}

void MessageRow::enterEvent(QEnterEvent *event) {
    m_hover = true;
    updateEditButton();
    updateRetryButton();
    QWidget::enterEvent(event);
}

void MessageRow::leaveEvent(QEvent *event) {
    m_hover = false;
    updateEditButton();
    updateRetryButton();
    QWidget::leaveEvent(event);
}

bool MessageRow::eventFilter(QObject *watched, QEvent *event) {
    if ((watched == m_editButton || watched == m_retryButton) && event->type() == QEvent::FocusIn) {
        m_focusWithin = true;
        updateEditButton();
        updateRetryButton();
    } else if ((watched == m_editButton || watched == m_retryButton) && event->type() == QEvent::FocusOut) {
        QTimer::singleShot(0, this, [this]() { updateFocusWithin(); });
    }
    if (watched == m_editBox && event->type() == QEvent::KeyPress) {
        auto *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape) {
            userCancel();
            return true;
        }
        if ((key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter) && (key->modifiers() & Qt::ControlModifier)) {
            userSave();   // Ctrl on Windows/Linux, Cmd on macOS
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}
