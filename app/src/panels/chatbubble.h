#pragma once

#include <QFrame>

#include <QSize>
#include <QString>

class QLabel;

// A single chat bubble — user (right-aligned, glass-fill-hover) or assistant
// (left-aligned, glass-fill + 2px accent-primary left border). The streaming
// cursor is toggled externally via setCursorVisible().
class ChatBubble : public QFrame {
    Q_OBJECT

public:
    enum Role { User, Assistant };

    explicit ChatBubble(Role role, const QString &text, QWidget *parent = nullptr);

    Role role() const { return m_role; }
    QString text() const { return m_text; }

    void appendText(const QString &delta);
    void setText(const QString &text);
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
};