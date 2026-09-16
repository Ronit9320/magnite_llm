#include "panels/chatbubble.h"

#include <QFontMetrics>
#include <QLabel>

namespace {
    // Keep these in sync with the bubble QSS (padding + 1px border each side).
    constexpr int kPadX = 14;
    constexpr int kPadY = 10;
    constexpr int kFrame = 1;
    // Max label width inside the bubble, mirrors setMaximumWidth(680).
    constexpr int kMaxInner = 680 - 2 * kPadX - 2 * kFrame;
    constexpr int kMinInner = 16;
}

ChatBubble::ChatBubble(Role role, const QString &text, QWidget *parent)
    : QFrame(parent)
    , m_role(role)
    , m_text(text)
    , m_content(nullptr)
{
    setObjectName(role == User ? QStringLiteral("chatBubbleUser")
                               : QStringLiteral("chatBubbleAssistant"));
    setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    setMaximumWidth(680);

    m_content = new QLabel(this);
    m_content->setObjectName(QStringLiteral("chatBubbleText"));
    m_content->setWordWrap(true);
    m_content->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_content->setMargin(0);

    if (!text.isEmpty())
        m_content->setText(text);
    remeasure();
}

QSize ChatBubble::measure(const QString &text) const {
    QFontMetrics fm = m_content->fontMetrics();
    if (text.isEmpty())
        return QSize(kPadX * 2 + kFrame * 2 + kMinInner,
                     fm.height() + kPadY * 2 + kFrame * 2);

    const QStringList words = text.split(QLatin1Char(' '),
                                         Qt::KeepEmptyParts);
    int spaceW = fm.horizontalAdvance(QLatin1Char(' '));
    int lineW = 0;
    int lines = 1;
    int widest = 0;
    for (const QString &word : words) {
        const int ww = fm.horizontalAdvance(word);
        const int candidate = (lineW ? lineW + spaceW : 0) + ww;
        if (candidate <= kMaxInner) {
            lineW = candidate;
        } else {
            lineW = ww;
            ++lines;
        }
        widest = qMax(widest, lineW);
    }
    const int innerW = qBound(kMinInner, widest, kMaxInner);
    return QSize(innerW + kPadX * 2 + kFrame * 2,
                 lines * fm.height() + kPadY * 2 + kFrame * 2);
}

void ChatBubble::remeasure() {
    const QSize s = measure(m_content->text());
    setFixedSize(s);
    m_content->setGeometry(kPadX + kFrame, kPadY + kFrame,
                           s.width() - 2 * kPadX - 2 * kFrame,
                           s.height() - 2 * kPadY - 2 * kFrame);
    updateGeometry();
}

void ChatBubble::appendText(const QString &delta) {
    m_text += delta;
    m_content->setText(m_text);
    remeasure();
}

void ChatBubble::setText(const QString &text) {
    m_text = text;
    m_content->setText(m_text);
    remeasure();
}

void ChatBubble::setCursorVisible(bool show) {
    m_content->setText(show ? (m_text + QStringLiteral("\u258D")) : m_text);
    remeasure();
}