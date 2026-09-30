#include "panels/chatbubble.h"

#include <QFontMetrics>
#include <QEvent>
#include <QLabel>
#include <QTextDocument>
#include <QTextOption>
#include <QApplication>
#include <QClipboard>
#include <QMenu>
#include <cmath>

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
    m_content->setTextFormat(Qt::PlainText);
    if (parent) parent->installEventFilter(this);
    m_content->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
    m_content->setAccessibleName(role == User ? "Your message" : "Model response");
    m_content->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(m_content,&QWidget::customContextMenuRequested,this,[this](const QPoint &position) {
        QMenu menu(this);
        auto *copy = menu.addAction("Copy message");
        if (menu.exec(m_content->mapToGlobal(position)) == copy) QApplication::clipboard()->setText(m_text);
    });
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

    const int available = parentWidget() ? parentWidget()->width() - 48 : 680;
    const int innerWidth = qBound(kMinInner, available - 2 * kPadX - 2 * kFrame, kMaxInner);
    int naturalWidth = kMinInner;
    for (const QString &line : text.split('\n')) naturalWidth = qMax(naturalWidth,fm.horizontalAdvance(line));
    const int wrapWidth = qMin(innerWidth,naturalWidth+1);
    QTextDocument document;
    document.setDocumentMargin(0);
    document.setDefaultFont(m_content->font());
    QTextOption option;
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    document.setDefaultTextOption(option);
    document.setPlainText(text);
    document.setTextWidth(wrapWidth);
    return QSize(wrapWidth + 2*kPadX + 2*kFrame,
                 int(std::ceil(document.size().height())) + 2*kPadY + 2*kFrame);

}

void ChatBubble::remeasure() {
    ensurePolished();
    m_content->ensurePolished();
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
bool ChatBubble::eventFilter(QObject *watched, QEvent *event) {
    if (watched == parentWidget() && event->type() == QEvent::Resize) remeasure();
    return QFrame::eventFilter(watched, event);
}
