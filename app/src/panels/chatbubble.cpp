#include "panels/chatbubble.h"

#include "theme.h"
#include "trainingduration.h"
#include <QPushButton>
#include <QSettings>

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
    // The thinking block: a 2px rule plus 10px of padding on its left (see QLabel#chatThinkingText).
    constexpr int kThinkIndent = 12;
    constexpr int kHeaderHeight = 24;
    constexpr int kGapAboveThinking = 6;
    constexpr int kGapAboveAnswer = 8;

    int documentHeight(const QString &text, const QFont &font, int width) {
        QTextDocument document;
        document.setDocumentMargin(0);
        document.setDefaultFont(font);
        QTextOption option;
        option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
        document.setDefaultTextOption(option);
        document.setPlainText(text);
        document.setTextWidth(width);
        return int(std::ceil(document.size().height()));
    }

    int widestLine(const QString &text, const QFontMetrics &metrics) {
        int widest = 0;
        for (const QString &line : text.split('\n')) widest = qMax(widest, metrics.horizontalAdvance(line));
        return widest;
    }
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
        if (menu.exec(m_content->mapToGlobal(position)) == copy)
            QApplication::clipboard()->setText(m_parsed.answer.isEmpty() ? m_text : m_parsed.answer);
    });
    m_content->setMargin(0);

    if (role == Assistant) {
        m_thinkToggle = new QPushButton(this);
        m_thinkToggle->setObjectName(QStringLiteral("thinkToggle"));
        m_thinkToggle->setFlat(true);
        m_thinkToggle->setCursor(Qt::PointingHandCursor);
        m_thinkToggle->hide();
        // Thinking is hidden unless the user last chose to show it; clicking the header makes that choice
        // for the replies that follow (and for the next launch).
        m_thinkExpanded = QSettings().value(QStringLiteral("chat/showThinking"), false).toBool();
        connect(m_thinkToggle, &QPushButton::clicked, this, [this]() {
            setThinkingExpanded(!m_thinkExpanded);
            QSettings().setValue(QStringLiteral("chat/showThinking"), m_thinkExpanded);
        });

        m_thinkText = new QLabel(this);
        m_thinkText->setObjectName(QStringLiteral("chatThinkingText"));
        m_thinkText->setWordWrap(true);
        m_thinkText->setIndent(0);   // a bordered label otherwise adds its own indent, which the measuring does not know about
        m_thinkText->setTextFormat(Qt::PlainText);
        m_thinkText->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard);
        m_thinkText->setAccessibleName(QStringLiteral("Model thinking"));
        m_thinkText->hide();

        // A bubble created empty is a reply about to stream: time its thinking from now.
        if (text.isEmpty()) m_clock.start();
    }

    refresh();
}

QString ChatBubble::thinkingHeader() const {
    if (!m_showHeader) return QString();
    if (m_streaming && m_parsed.open) return QStringLiteral("Thinking…");
    if (m_thinkMs < 0) return QStringLiteral("Thought process");
    return m_thinkMs < 1000 ? QStringLiteral("Thought for <1s")
                            : QStringLiteral("Thought for ") + TrainingTime::duration(m_thinkMs / 1000.0);
}

void ChatBubble::refresh() {
    if (m_role == Assistant) {
        m_parsed = Reasoning::parse(m_text);
    } else {
        m_parsed = Reasoning::Parsed();
        m_parsed.answer = m_text;
    }
    if (m_clock.isValid() && m_thinkMs < 0 && m_parsed.hasThinking && !m_parsed.open) m_thinkMs = m_clock.elapsed();

    const bool live = m_streaming && m_parsed.open;
    m_showHeader = m_thinkToggle && (m_parsed.hasThinking || live);

    const QString cursor = m_cursor ? QStringLiteral("▍") : QString();
    m_content->setText(m_parsed.answer + (m_parsed.open ? QString() : cursor));
    if (m_thinkToggle) {
        m_thinkText->setText(m_parsed.thinking + (m_parsed.open ? cursor : QString()));
        m_thinkToggle->setVisible(m_showHeader);
        if (m_showHeader) {
            m_thinkToggle->setText(thinkingHeader());
            m_thinkToggle->setIcon(Theme::coloredIcon(m_thinkExpanded ? QStringLiteral("chevron-down")
                                                                      : QStringLiteral("chevron-right"),
                                                      QColor(Theme::textMuted()), 14));
            const QString action = m_thinkExpanded ? QStringLiteral("Hide thinking") : QStringLiteral("Show thinking");
            m_thinkToggle->setAccessibleName(action);
            m_thinkToggle->setToolTip(action);
        }
    }
    remeasure();
}

QSize ChatBubble::layoutParts(bool apply) {
    const QString answer = m_content->text();
    const QFontMetrics fm = m_content->fontMetrics();
    const bool expanded = m_showHeader && m_thinkExpanded && !m_thinkText->text().isEmpty();
    const bool showAnswer = !answer.isEmpty() || !m_showHeader;

    if (!m_showHeader && answer.isEmpty()) {
        if (apply) m_content->setGeometry(kPadX + kFrame, kPadY + kFrame, kMinInner, fm.height());
        return QSize(kPadX * 2 + kFrame * 2 + kMinInner, fm.height() + kPadY * 2 + kFrame * 2);
    }

    const QWidget *reference = m_reference ? m_reference.data() : parentWidget();
    const int available = reference ? reference->width() - 48 : 680;
    const int innerWidth = qBound(kMinInner, available - 2 * kPadX - 2 * kFrame, kMaxInner);

    int naturalWidth = qMax(kMinInner, widestLine(answer, fm));
    int headerWidth = 0;
    if (m_showHeader) {
        m_thinkToggle->ensurePolished();
        // 14px icon, a gap, the label, and a little slack so it is never elided.
        headerWidth = 14 + 6 + QFontMetrics(m_thinkToggle->font()).horizontalAdvance(m_thinkToggle->text()) + 4;
        naturalWidth = qMax(naturalWidth, headerWidth);
    }
    if (expanded) naturalWidth = qMax(naturalWidth, widestLine(m_thinkText->text(), QFontMetrics(m_thinkText->font())) + kThinkIndent);
    const int wrapWidth = qMin(innerWidth, naturalWidth + 1);

    int y = 0;
    const int x = kPadX + kFrame;
    const int top = kPadY + kFrame;
    if (m_showHeader) {
        if (apply) m_thinkToggle->setGeometry(x, top, qMin(headerWidth, wrapWidth), kHeaderHeight);
        y += kHeaderHeight;
    }
    if (expanded) {
        y += kGapAboveThinking;
        const int height = documentHeight(m_thinkText->text(), m_thinkText->font(), wrapWidth - kThinkIndent);
        if (apply) m_thinkText->setGeometry(x, top + y, wrapWidth, height);
        y += height;
    }
    if (apply && m_thinkText) m_thinkText->setVisible(expanded);
    int answerHeight = 0;
    if (showAnswer) {
        if (m_showHeader) y += kGapAboveAnswer;
        answerHeight = documentHeight(answer, m_content->font(), wrapWidth);
        if (apply) m_content->setGeometry(x, top + y, wrapWidth, answerHeight);
        y += answerHeight;
    }
    if (apply) m_content->setVisible(showAnswer);
    return QSize(wrapWidth + 2 * kPadX + 2 * kFrame, y + 2 * kPadY + 2 * kFrame);
}

void ChatBubble::remeasure() {
    ensurePolished();
    m_content->ensurePolished();
    if (m_thinkText) m_thinkText->ensurePolished();
    setFixedSize(layoutParts(true));
    updateGeometry();
}

void ChatBubble::setWidthReference(QWidget *reference) {
    m_reference = reference;
    if (reference) reference->installEventFilter(this);
    remeasure();
}

void ChatBubble::setText(const QString &text) {
    m_text = text;
    refresh();
}

void ChatBubble::setCursorVisible(bool show) {
    m_cursor = show;
    refresh();
}

void ChatBubble::setStreaming(bool streaming) {
    if (m_streaming == streaming) return;
    m_streaming = streaming;
    refresh();
}

void ChatBubble::setThinkingExpanded(bool expanded) {
    m_thinkExpanded = expanded;
    refresh();
}

bool ChatBubble::eventFilter(QObject *watched, QEvent *event) {
    if ((watched == parentWidget() || (m_reference && watched == m_reference.data())) && event->type() == QEvent::Resize) remeasure();
    return QFrame::eventFilter(watched, event);
}
