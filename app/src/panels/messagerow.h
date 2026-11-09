#pragma once

#include "panels/chatbubble.h"
#include "panels/chattree.h"

#include <QWidget>

class QEnterEvent;
class QFocusEvent;
class QPlainTextEdit;
class QPushButton;
class VersionSwitcher;

// One message in the transcript: its bubble, aligned to the right for the user and to the
// left for the model, with a version switcher underneath when the message has several
// versions. User rows also have an edit pencil (shown on hover) and an inline editor. It
// knows the conversation-tree node it shows (0 until a streaming reply is committed).
class MessageRow : public QWidget {
    Q_OBJECT

public:
    MessageRow(ChatBubble::Role role, ChatTree::NodeId node, const QString &text, QWidget *parent);

    ChatBubble *bubble() const { return m_bubble; }
    ChatTree::NodeId node() const { return m_node; }
    void setNode(ChatTree::NodeId node);

    // `index` is 0-based; the switcher is hidden unless `total` is above 1.
    void setVersion(int index, int total);
    // false disables the version arrows and the edit pencil (a reply is streaming, a send is being prepared…).
    void setActionsEnabled(bool enabled);
    // User rows only: true for a message that never got a reply, so saving it unchanged sends it again
    // instead of counting as a cancel.
    void setResendable(bool resendable) { m_resendable = resendable; }
    // Reply rows only: whether the retry button can be used (it only appears on hover).
    void setRetryAvailable(bool available);
    // Turns only the version arrows on or off (they also stay off while an editor is open elsewhere).
    void setSwitcherEnabled(bool enabled);

    // User rows only. Swaps the bubble for an inline editor holding `text`. Silent: no signal.
    void beginEdit(const QString &text);
    // Closes the editor without a signal (used when another editor opens or the panel resets).
    void cancelEdit();
    bool isEditing() const { return m_editing; }

signals:
    void versionStep(ChatTree::NodeId node, int delta);
    void retryClicked(ChatTree::NodeId node);                        // the retry button was clicked
    void editRequested(ChatTree::NodeId node);                       // the pencil was clicked
    void editSaved(ChatTree::NodeId node, const QString &text);      // trimmed, non-empty and changed; the editor is already closed
    void editCancelled(ChatTree::NodeId node);                       // Esc, Cancel, or an empty/unchanged Save

protected:
    void enterEvent(QEnterEvent *event) override;
    void leaveEvent(QEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void focusOutEvent(QFocusEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void updateEditButton();
    void updateRetryButton();
    void updateFocusPolicy();       // a row is a tab stop only while it has an action to offer
    void updateFocusWithin();       // keyboard focus on the row or one of its buttons keeps the buttons shown
    void closeEditor();
    void userCancel();
    void userSave();
    void resizeEditor();

    ChatBubble::Role m_role;
    QWidget *m_lineHost;          // holds the bubble and the pencil
    ChatBubble *m_bubble;
    QPushButton *m_editButton;    // null on reply rows
    QPushButton *m_retryButton;   // null on user rows
    QWidget *m_editor;            // null on reply rows
    QPlainTextEdit *m_editBox;
    VersionSwitcher *m_switcher;
    ChatTree::NodeId m_node;
    bool m_hover = false;
    bool m_focusWithin = false;
    bool m_editing = false;
    bool m_actionsEnabled = true;
    bool m_retryAvailable = false;
    bool m_resendable = false;
};
