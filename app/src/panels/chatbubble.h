#pragma once

#include "reasoning.h"

#include <QElapsedTimer>
#include <QFrame>
#include <QPointer>

#include <QSize>
#include <QString>

class QLabel;
class QPushButton;

// A single chat bubble — user (right-aligned, surface-fill-hover) or assistant
// (left-aligned, no fill). The streaming cursor is toggled externally via setCursorVisible().
// An assistant reply that carries <think>…</think> shows the thinking behind a collapsible
// "Thought for 4s" header above the answer; text() still returns the whole reply, tags included.
class ChatBubble : public QFrame {
    Q_OBJECT

public:
    enum Role { User, Assistant };

    explicit ChatBubble(Role role, const QString &text, QWidget *parent = nullptr);

    Role role() const { return m_role; }
    QString text() const { return m_text; }
    // The reply without its thinking (what Copy puts on the clipboard).
    QString answerText() const { return m_parsed.answer; }
    bool hasThinking() const { return m_showHeader; }
    bool thinkingExpanded() const { return m_thinkExpanded; }
    QString thinkingHeader() const;

    void setText(const QString &text);
    // Size the bubble against `reference`'s width instead of its parent's (a row that wraps
    // the bubble would otherwise make it narrower than the chat area it belongs to).
    void setWidthReference(QWidget *reference);
    void setCursorVisible(bool show);
    // True while the reply is still being generated: an unclosed <think> then reads "Thinking…"
    // and is shown open; once false it is a finished (or stopped) thought.
    void setStreaming(bool streaming);
    // Opens or closes the thinking, as clicking its header does.
    void setThinkingExpanded(bool expanded);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    // Deterministic size of the thinking header, thinking and answer at the current font: wraps by
    // word at the bubble's maximum width and returns the bubble size. With `apply` it also places
    // the three parts. Layout engine independent — no heightForWidth coupling.
    QSize layoutParts(bool apply);
    void remeasure();
    void refresh();           // re-reads m_text: splits thinking from answer, fills the labels, resizes

    QPushButton *m_thinkToggle = nullptr;   // assistant bubbles only; hidden without thinking
    QLabel *m_thinkText = nullptr;
    Reasoning::Parsed m_parsed;
    bool m_showHeader = false;
    bool m_cursor = false;
    bool m_streaming = false;
    bool m_thinkExpanded = false;
    bool m_thinkChosen = false;             // the user clicked the header: stop opening/closing it automatically
    QElapsedTimer m_clock;                  // started for a bubble that is created empty (a reply about to stream)
    qint64 m_thinkMs = -1;

    Role m_role;
    QString m_text;
    QLabel *m_content;
    QPointer<QWidget> m_reference;
};