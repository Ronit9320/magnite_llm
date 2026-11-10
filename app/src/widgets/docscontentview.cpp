#include "widgets/docscontentview.h"
#include "theme.h"
#include <QAbstractTextDocumentLayout>
#include <QApplication>
#include <QLayout>
#include <QPainter>
#include <QResizeEvent>
#include <QScrollBar>
#include <QSettings>
#include <QTextDocument>
#include <QTextOption>
#include <QWheelEvent>
#include <cmath>

DocsCard::DocsCard(QWidget *parent) : Button(parent) {
    setObjectName("docsCard");
    setText("");
    setMinimumWidth(0);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    auto policy = sizePolicy();
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
}
QSize DocsCard::sizeHint() const { return layout() ? layout()->sizeHint() : Button::sizeHint(); }
QSize DocsCard::minimumSizeHint() const { return QSize(0, layout() ? layout()->minimumSize().height() : 40); }
int DocsCard::heightForWidth(int width) const {
    if (!layout())
        return Button::heightForWidth(width);
    return layout()->hasHeightForWidth() ? layout()->heightForWidth(width) : layout()->sizeHint().height();
}
DocsTextView::DocsTextView(QWidget *parent, bool expand) : QTextBrowser(parent), m_expand(expand) {
    setObjectName("docsRichText");
    setFrameShape(QFrame::NoFrame);
    setOpenLinks(false);
    setOpenExternalLinks(false);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(expand ? Qt::ScrollBarAlwaysOff : Qt::ScrollBarAsNeeded);
    setSizePolicy(QSizePolicy::Expanding, expand ? QSizePolicy::Fixed : QSizePolicy::Expanding);
    setMinimumWidth(0);
    setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::TextSelectableByKeyboard | Qt::LinksAccessibleByMouse |
                            Qt::LinksAccessibleByKeyboard);
    document()->setDocumentMargin(0);
    auto option = document()->defaultTextOption();
    option.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
    document()->setDefaultTextOption(option);
    connect(document()->documentLayout(), &QAbstractTextDocumentLayout::documentSizeChanged, this,
            [this] { reflow(); });
}
void DocsTextView::applyTextTheme() {
    const int size = qBound(0, QSettings().value("appearance/interfaceSize", 1).toInt(), 2);
    const qreal scale = size == 0 ? .92 : size == 2 ? 1.14 : 1.;
    QFont font(Theme::bodyFont());
    font.setPixelSize(qRound(14 * scale));
    document()->setDefaultFont(font);
    document()->setDefaultStyleSheet(QString("body { color:%1; font-family:'%2'; } p { margin-top:0px; "
                                             "margin-bottom:14px; line-height:180%; } "
                                             "li { margin-bottom:8px; line-height:180%; } strong,b { "
                                             "color:%3; font-weight:600; } "
                                             "a { color:%4; text-decoration:underline; } table { "
                                             "border-collapse:collapse; } "
                                             "td,th { border-bottom:1px solid %5; padding:9px; } th { "
                                             "color:%3; font-weight:600; } "
                                             "code,pre { font-family:'%6'; background-color:%7; } "
                                             ".article-callout { background-color:%7; margin:16px 0; "
                                             "padding:14px; }")
                                         .arg(Theme::textSecondary(), Theme::bodyFont(), Theme::textPrimary(),
                                              Theme::accentLabel(), Theme::surfaceBorder(), Theme::monoFont(),
                                              Theme::surfaceFill()));
    QPalette colors = palette();
    colors.setColor(QPalette::Base, Qt::transparent);
    colors.setColor(QPalette::Text, QColor(Theme::textSecondary()));
    setPalette(colors);
}
void DocsTextView::setContent(const QString &source) {
    applyTextTheme();
    QString html = source;
    html.replace("<table>", "<table width=\"100%\" cellspacing=\"0\" cellpadding=\"7\">");
    setHtml(html);
    reflow();
}
void DocsTextView::setReferenceMarkdown(const QString &markdown) {
    applyTextTheme();
    setMarkdown(markdown);
    reflow();
}
void DocsTextView::reflow() {
    if (!m_expand || m_reflowing || viewport()->width() < 1)
        return;
    m_reflowing = true;
    document()->setTextWidth(viewport()->width());
    // Stylesheet padding and frame insets reduce the actual text viewport.
    // Include them so an expanding reader never hides its final lines.
    const int insets = qMax(0, this->height() - viewport()->height());
    const int height = qMax(1, int(std::ceil(document()->documentLayout()->documentSize().height())) + insets + 3);
    if (height != this->height())
        setFixedHeight(height);
    m_reflowing = false;
}
void DocsTextView::resizeEvent(QResizeEvent *event) {
    QTextBrowser::resizeEvent(event);
    reflow();
}
void DocsTextView::wheelEvent(QWheelEvent *event) {
    if (m_expand)
        event->ignore();
    else
        QTextBrowser::wheelEvent(event);
}
QVariant DocsTextView::loadResource(int, const QUrl &) { return {}; }

DocsDiagram::DocsDiagram(Kind kind, QWidget *parent) : QWidget(parent), m_kind(kind) {
    setObjectName("docsConceptDiagram");
    auto policy = QSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    policy.setHeightForWidth(true);
    setSizePolicy(policy);
    setMinimumWidth(0);
    setAccessibleName(kind == Distribution ? "Illustrative dataset distribution" : "Concept illustration");
    if (kind == Learning)
        setAccessibleDescription("Context tokens lead to a model prediction over possible next tokens.");
    if (kind == Tokens)
        setAccessibleDescription("Original text is represented by a tokenizer and "
                                 "vocabulary as token IDs.");
    if (kind == LoRA)
        setAccessibleDescription("Frozen base weights plus a trainable LoRA "
                                 "adapter form the adapted model.");
    if (kind == Data)
        setAccessibleDescription("Your examples become model inputs and contribute "
                                 "to the learning signal.");
    if (kind == Distribution)
        setAccessibleDescription("Illustrative data, not your files: 800 refund requests, 180 setup "
                                 "questions, and 20 other situations.");
}
QSize DocsDiagram::sizeHint() const { return QSize(430, m_kind == Distribution ? 160 : 90); }
int DocsDiagram::heightForWidth(int width) const { return m_kind == Distribution ? 160 : width < 390 ? 230 : 90; }
void DocsDiagram::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    QFont body(Theme::bodyFont());
    body.setPixelSize(11);
    p.setFont(body);
    if (m_kind == Distribution) {
        p.setPen(QColor(Theme::surfaceBorder()));
        p.setBrush(QColor(Theme::surfaceFill()));
        p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 8, 8);
        p.setPen(QColor(Theme::textMuted()));
        p.drawText(QRect(15, 12, width() - 30, 28), Qt::AlignLeft | Qt::AlignVCenter,
                   "Illustrative dataset · 1,000 support examples");
        const QStringList names = {"Refund requests", "Setup questions", "Other situations"};
        const int values[] = {800, 180, 20};
        const int labelWidth = qMin(140, qMax(100, width() / 3)), chartLeft = 15 + labelWidth,
                  chartWidth = qMax(1, width() - chartLeft - 58);
        for (int i = 0; i < 3; ++i) {
            const int y = 51 + i * 32;
            p.setPen(QColor(Theme::textSecondary()));
            p.drawText(QRect(15, y, labelWidth - 5, 23), Qt::AlignVCenter, names[i]);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(Theme::accentPrimary()));
            p.drawRoundedRect(QRectF(chartLeft, y + 8, qMax(3., chartWidth * values[i] / 1000.), 6), 3, 3);
            p.setPen(QColor(Theme::textMuted()));
            p.drawText(QRect(width() - 48, y, 32, 23), Qt::AlignRight | Qt::AlignVCenter, QString::number(values[i]));
        }
        return;
    }
    QStringList titles, subtitles;
    switch (m_kind) {
    case Learning:
        titles = {"Context tokens", "Model prediction", "Possible next tokens"};
        break;
    case Tokens:
        titles = {"Original text", "Tokenizer + vocabulary", "Token IDs"};
        break;
    case LoRA:
        titles = {"Base weights", "LoRA adapter", "Adapted model"};
        subtitles = {"Frozen", "Trainable", "Base + adapter"};
        break;
    case Data:
        titles = {"Your examples", "Model inputs", "Learning signal"};
        subtitles = {"Content · coverage · consistency", "Tokens · formatting · length", "What the objective rewards"};
        break;
    default:
        break;
    }
    const bool vertical = width() < 390;
    const qreal gap = vertical ? 19 : 25;
    const qreal boxWidth = vertical ? width() - 2 : (width() - 2 - 2 * gap) / 3;
    const qreal boxHeight = vertical ? (height() - 2 - 2 * gap) / 3 : height() - 14;
    for (int i = 0; i < 3; ++i) {
        const QRectF box(vertical ? 1 : 1 + i * (boxWidth + gap), vertical ? 1 + i * (boxHeight + gap) : 7, boxWidth,
                         boxHeight);
        p.setPen(QPen(QColor(Theme::surfaceBorder()), 1));
        p.setBrush(QColor(m_kind == Data && i < 2 ? Theme::canvasBackground() : Theme::surfaceFill()));
        if (m_kind == Data && i == 2)
            p.setPen(QPen(QColor(Theme::surfaceBorder()), 1, Qt::DashLine));
        p.drawRoundedRect(box, 7, 7);
        p.setPen(QColor(Theme::textPrimary()));
        QRectF text = box.adjusted(8, 8, -8, -8);
        if (!subtitles.isEmpty())
            text.setBottom(text.bottom() - (m_kind == Data ? 27 : 16));
        if (m_kind == Data) {
            const QStringList icons{"database", "file-text", "layers"};
            Theme::coloredIcon(icons[i], QColor(i == 2 ? Theme::accentLabel() : Theme::textSecondary()), 14)
                .paint(&p, QRect(qRound(box.left() + 10), qRound(box.top() + 15), 14, 14));
            text.setLeft(text.left() + 25);
        }
        p.drawText(text, Qt::AlignCenter | Qt::TextWordWrap, titles.value(i));
        if (!subtitles.isEmpty()) {
            p.setPen(QColor(Theme::textMuted()));
            QFont small = body;
            small.setPixelSize(9);
            p.setFont(small);
            p.drawText(QRectF(box.left() + 5, box.bottom() - (m_kind == Data ? 34 : 25), box.width() - 10,
                              m_kind == Data ? 28 : 17),
                       Qt::AlignCenter | Qt::TextWordWrap, subtitles.value(i));
            p.setFont(body);
        }
        if (i < 2) {
            p.setPen(QColor(Theme::accentLabel()));
            const QRectF arrow =
                vertical ? QRectF(0, box.bottom(), width(), gap) : QRectF(box.right(), 0, gap, height());
            p.drawText(arrow, Qt::AlignCenter, m_kind == LoRA && i == 0 ? "+" : vertical ? "↓" : "→");
        }
    }
}
