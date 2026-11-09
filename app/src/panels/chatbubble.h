#pragma once

#include <QFrame>
#include <QPointer>

#include <QSize>
#include <QString>

class QLabel;

// A single chat bubble — user (right-aligned, surface-fill-hover) or assistant
// (left-aligned, surface-fill + 2px accent-primary left border). The streaming
// cursor is toggled externally via setCursorVisible().
class ChatBubble : public QFrame {
    Q_OBJECT

public:
    enum Role { User, Assistant };

    explicit ChatBubble(Role role, const QString &text, QWidget *parent = nullptr);

    Role role() const { return m_role; }
    QString text() const { return m_text; }

    void setText(const QString &text);
    // Size the bubble against `reference`'s width instead of its parent's (a row that wraps
    // the bubble would otherwise make it narrower than the chat area it belongs to).
    void setWidthReference(QWidget *reference);
    void setCursorVisible(bool show);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // Deterministic size for `text` at the current font: wraps by word at the
    // bubble's maximum width and returns (bubble, bubble) size. Layout engine
    // independent — no heightForWidth coupling.
    QSize measure(const QString &text) const;
    void remeasure();

    Role m_role;
    QString m_text;
    QLabel *m_content;
    QPointer<QWidget> m_reference;
};