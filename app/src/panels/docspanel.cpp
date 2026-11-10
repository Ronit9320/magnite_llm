#include "panels/docspanel.h"
#include "theme.h"
#include "widgets/button.h"
#include "widgets/combobox.h"
#include "widgets/docscontentview.h"
#include "widgets/motion.h"
#include "widgets/settingswindow.h"
#include <QApplication>
#include <QBoxLayout>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QGraphicsOpacityEffect>
#include <QGridLayout>
#include <QHideEvent>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QShortcut>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QStyle>
#include <QTimer>
#include <QUrlQuery>
#include <QVBoxLayout>

namespace {
QLabel *text(const QString &value, const char *name = "docsBody") {
    auto *label = new QLabel(value);
    label->setObjectName(name);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setMinimumWidth(0);
    return label;
}
QLabel *rich(const QString &value, const char *name = "docsBody") {
    auto *label = text(value, name);
    label->setTextFormat(Qt::RichText);
    return label;
}
void transparentToMouse(QWidget *host) {
    for (auto *label : host->findChildren<QLabel *>())
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
}
QLabel *symbol(const QString &name, int size = 18) {
    auto *label = new QLabel;
    label->setObjectName("docsIcon");
    label->setProperty("themeIcon", name);
    label->setPixmap(Theme::coloredIcon(name, QColor(Theme::accentLabel()), size).pixmap(size, size));
    label->setFixedSize(size, size);
    return label;
}
QFrame *frame(const char *name) {
    auto *result = new QFrame;
    result->setObjectName(name);
    return result;
}
void rule(QVBoxLayout *layout) {
    auto *line = frame("docsRule");
    line->setFixedHeight(1);
    layout->addWidget(line);
}
QString literal(QString value) { return value.replace('&', "&&"); }
class NavScrim : public QWidget {
  public:
    NavScrim(QWidget *parent, std::function<void()> close) : QWidget(parent), m_close(std::move(close)) {
        hide();
        setAccessibleName("Close topic navigation");
    }

  protected:
    void paintEvent(QPaintEvent *) override {
        QPainter p(this);
        QColor color(Theme::brandInk());
        color.setAlpha(85);
        p.fillRect(rect(), color);
    }
    void mousePressEvent(QMouseEvent *event) override {
        if (event->button() == Qt::LeftButton)
            m_close();
    }

  private:
    std::function<void()> m_close;
};
int matchingClose(const QString &html, int start, const QString &tag) {
    QRegularExpression tags("<" + tag + "\\b[^>]*>|</" + tag + ">", QRegularExpression::CaseInsensitiveOption);
    auto matches = tags.globalMatch(html, start);
    int depth = 0;
    while (matches.hasNext()) {
        const auto match = matches.next();
        if (match.captured().startsWith("</"))
            --depth;
        else
            ++depth;
        if (depth == 0)
            return match.capturedEnd();
    }
    return html.size();
}
} // namespace
DocsPanel::DocsPanel(QWidget *parent) : BasePanel("Docs", parent, true) {
    setObjectName("docsPanel");
    setAttribute(Qt::WA_StyledBackground);
    openExternalUrl = [](const QUrl &url) { return QDesktopServices::openUrl(url); };
    chooseExampleDestination = [this](const QString &filename) {
        return QFileDialog::getSaveFileName(this, "Save dataset example", filename, "Dataset examples (*.jsonl *.csv)");
    };
    m_library.load();
    QSettings settings;
    for (const auto &id : settings.value("docs/savedArticles").toStringList())
        if (m_library.article(id))
            m_saved.insert(id);
    const auto groups = m_library.groups();
    for (const auto &group : settings.value("docs/collapsedSubjects").toStringList())
        if (groups.contains(group))
            m_collapsed.insert(group);
    m_large = settings.value("docs/textSize", "standard").toString() == "large";
    m_lastArticle = settings.value("docs/lastArticle").toString();
    m_saveTimer = new QTimer(this);
    m_saveTimer->setSingleShot(true);
    m_saveTimer->setInterval(180);
    connect(m_saveTimer, &QTimer::timeout, this, &DocsPanel::savePosition);
    m_resizeTimer = new QTimer(this);
    m_resizeTimer->setSingleShot(true);
    m_resizeTimer->setInterval(45);
    connect(m_resizeTimer, &QTimer::timeout, this, [this] {
        updateResponsive();
        if (m_layoutClass != layoutClass())
            render(true);
    });
    setupUi();
    m_history << m_route;
    m_ready = true;
    render();
    qApp->installEventFilter(this);
}
DocsPanel::~DocsPanel() {
    savePosition();
    qApp->removeEventFilter(this);
}
QPushButton *DocsPanel::action(const QString &value, const std::function<void()> &callback, const char *name) {
    auto *button = new Button(literal(value));
    button->setObjectName(name);
    button->setAutoDefault(false);
    button->setAccessibleName(value);
    connect(button, &QPushButton::clicked, this, callback);
    return button;
}
void DocsPanel::setupUi() {
    auto *root = contentLayout();
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);
    auto *toolbar = frame("docsToolbar");
    auto *top = new QHBoxLayout(toolbar);
    top->setContentsMargins(22, 12, 22, 12);
    top->setSpacing(12);
    m_menu = action("☰", [this] { toggleNavigation(); }, "docsMenu");
    m_menu->setAccessibleName("Open topic navigation");
    m_menu->setFixedWidth(30);
    top->addWidget(m_menu);
    m_back = action("", [this] { goHistory(-1); }, "docsHistoryBack");
    m_back->setProperty("themeIcon", "chevron-left");
    m_back->setIcon(Theme::coloredIcon("chevron-left", QColor(Theme::textSecondary()), 14));
    m_back->setAccessibleName("Back in Docs");
    m_back->setToolTip("Back in Docs · Alt+Left");
    m_back->setFixedWidth(28);
    top->addWidget(m_back);
    m_forward = action("", [this] { goHistory(1); }, "docsHistoryForward");
    m_forward->setProperty("themeIcon", "chevron-right");
    m_forward->setIcon(Theme::coloredIcon("chevron-right", QColor(Theme::textSecondary()), 14));
    m_forward->setAccessibleName("Forward in Docs");
    m_forward->setToolTip("Forward in Docs · Alt+Right");
    m_forward->setFixedWidth(28);
    top->addWidget(m_forward);
    top->addWidget(text("Docs", "docsToolbarTitle"));
    m_toolbarSubtitle = text("/   Models, data & understanding.", "docsHint");
    m_toolbarSubtitle->setWordWrap(false);
    top->addWidget(m_toolbarSubtitle);
    top->addStretch();
    m_search = new QLineEdit;
    m_search->setObjectName("docsSearch");
    m_search->setPlaceholderText("Search concepts, data, parameters…");
    m_search->setAccessibleName("Search the reference library");
    m_search->setClearButtonEnabled(true);
    m_search->setMinimumWidth(100);
    m_search->setMaximumWidth(330);
    m_search->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    top->addWidget(m_search);
    connect(m_search, &QLineEdit::textEdited, this, [this](const QString &query) { showSearch(query); });
    connect(m_search, &QLineEdit::returnPressed, this, [this] {
        const QUrl link(m_search->text());
        if (link.scheme() == "magnite-docs" && !openLink(link))
            setStatus("This Docs link does not identify a bundled article and section.");
    });
    root->addWidget(toolbar);
    m_shell = new QWidget;
    m_shellLayout = new QHBoxLayout(m_shell);
    m_shellLayout->setContentsMargins(0, 0, 0, 0);
    m_shellLayout->setSpacing(0);
    root->addWidget(m_shell, 1);
    m_navigation = new QWidget(m_shell);
    m_navigation->setObjectName("docsNavigation");
    m_navigation->setAttribute(Qt::WA_StyledBackground);
    m_navigation->setFixedWidth(222);
    m_navigationBody = new QVBoxLayout(m_navigation);
    m_navigationBody->setContentsMargins(16, 23, 16, 17);
    m_navigationBody->setSpacing(5);
    m_shellLayout->addWidget(m_navigation);
    m_scroll = new QScrollArea;
    m_scroll->setObjectName("docsScroll");
    m_scroll->setFrameShape(QFrame::NoFrame);
    m_scroll->setWidgetResizable(true);
    m_scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_scroll->setMinimumWidth(0);
    m_shellLayout->addWidget(m_scroll, 1);
    connect(m_scroll->verticalScrollBar(), &QScrollBar::valueChanged, this, [this] {
        if (m_restoring)
            return;
        updateContents();
        m_saveTimer->start();
    });
    m_status = text("", "docsStatus");
    m_status->setAccessibleName("Docs status");
    m_status->hide();
    root->addWidget(m_status);
    m_scrim = new NavScrim(m_shell, [this] { closeNavigation(); });
    for (const auto &sequence : {QKeySequence(Qt::CTRL | Qt::Key_K), QKeySequence(Qt::ALT | Qt::Key_Left),
                                 QKeySequence(Qt::ALT | Qt::Key_Right)}) {
        auto *shortcut = new QShortcut(sequence, this);
        shortcut->setContext(Qt::WidgetWithChildrenShortcut);
        connect(shortcut, &QShortcut::activated, this, [this, sequence] {
            if (sequence == QKeySequence(Qt::CTRL | Qt::Key_K)) {
                closeNavigation();
                m_search->setFocus();
                m_search->selectAll();
            } else
                goHistory(sequence == QKeySequence(Qt::ALT | Qt::Key_Left) ? -1 : 1);
        });
    }
}
QString DocsPanel::currentArticleId() const { return m_route.kind == "article" ? m_route.id : QString(); }
int DocsPanel::mainWidth() const { return qMax(200, width() - ((!m_compact && !m_focus) ? 222 : 0)); }
QString DocsPanel::layoutClass() const {
    const int width = mainWidth();
    return QString("%1/%2/%3/%4")
        .arg(m_compact)
        .arg(m_focus)
        .arg(width >= 860)
        .arg(width >= 700   ? 3
             : width >= 500 ? 2
                            : 1);
}
void DocsPanel::navigate(const Route &route, bool replace) {
    savePosition();
    closeNavigation();
    if (route == m_route)
        return;
    if (m_focus && route.kind != "article") {
        m_focus = false;
        emit focusModeChanged(false);
    }
    if (replace) {
        if (!m_history.isEmpty())
            m_history[m_historyIndex] = route;
    } else {
        while (m_history.size() > m_historyIndex + 1)
            m_history.removeLast();
        m_history << route;
        m_historyIndex = m_history.size() - 1;
    }
    m_route = route;
    render();
}
bool DocsPanel::openArticle(const QString &id, const QString &section) {
    const auto *article = m_library.article(id);
    if (!article)
        return false;
    if (!section.isEmpty()) {
        bool found = false;
        for (const auto &s : article->sections)
            found |= s.id == section;
        if (!found)
            return false;
    }
    if (m_route.kind == "article" && m_route.id == id && !section.isEmpty()) {
        savePosition();
        Route route = m_route;
        route.section = section;
        if (route != m_route) {
            while (m_history.size() > m_historyIndex + 1)
                m_history.removeLast();
            m_history << route;
            m_historyIndex = m_history.size() - 1;
            m_route = route;
        }
        scrollToSection(section);
        m_back->setEnabled(m_historyIndex > 0);
        m_forward->setEnabled(m_historyIndex + 1 < m_history.size());
        return true;
    }
    Route route;
    route.kind = "article";
    route.id = id;
    route.section = section;
    navigate(route);
    return true;
}
bool DocsPanel::openLink(const QUrl &link) {
    QString id, section;
    return DocsLibrary::parseArticleLink(link, &id, &section) && openArticle(id, section);
}
void DocsPanel::showOverview() { navigate(Route{}); }
void DocsPanel::showLibrary(bool saved) {
    Route route;
    route.kind = saved ? "saved" : "library";
    navigate(route);
}
void DocsPanel::showSearch(const QString &query) {
    if (query.startsWith("magnite-docs:", Qt::CaseInsensitive))
        return;
    if (query.trimmed().isEmpty()) {
        if (m_route.kind == "search")
            goHistory(-1);
        return;
    }
    Route route;
    route.kind = "search";
    route.query = query;
    const bool replace = m_route.kind == "search";
    navigate(route, replace);
    m_search->setFocus();
}
void DocsPanel::goHistory(int direction) {
    const int index = m_historyIndex + direction;
    if (index < 0 || index >= m_history.size())
        return;
    savePosition();
    closeNavigation();
    m_historyIndex = index;
    m_route = m_history[index];
    if (m_focus && m_route.kind != "article") {
        m_focus = false;
        emit focusModeChanged(false);
    }
    render();
}
void DocsPanel::setStatus(const QString &message) {
    m_status->setText(message);
    m_status->setAccessibleDescription(message);
    m_status->setVisible(!message.isEmpty());
}
void DocsPanel::flushPosition() {
    m_saveTimer->stop();
    savePosition();
    QSettings().sync();
}
void DocsPanel::savePosition() {
    if (!m_ready || m_restoring || m_route.kind != "article" || !m_scroll->widget())
        return;
    auto *bar = m_scroll->verticalScrollBar();
    const double ratio = bar->maximum() > 0 ? double(bar->value()) / bar->maximum() : 0.;
    QSettings settings;
    settings.setValue("docs/positions/" + m_route.id, QVariantMap{{"offset", bar->value()},
                                                                  {"ratio", ratio},
                                                                  {"width", m_scroll->viewport()->width()},
                                                                  {"large", m_large},
                                                                  {"section", m_activeSection}});
    settings.setValue("docs/lastArticle", m_route.id);
    m_lastArticle = m_route.id;
}

void DocsPanel::renderNavigation() {
    const int previous = m_topicScroll ? m_topicScroll->verticalScrollBar()->value() : 0;
    while (auto *item = m_navigationBody->takeAt(0)) {
        if (item->widget()) {
            item->widget()->hide();
            item->widget()->deleteLater();
        }
        delete item;
    }
    m_groupButtons.clear();
    if (m_compact)
        m_navigationBody->addWidget(action("Explore Docs    ×", [this] { closeNavigation(); }, "docsCloseNavigation"));
    auto nav = [this](const QString &title, const QString &kind, const std::function<void()> &callback) {
        auto *b = action(title, callback, "docsNav");
        b->setProperty("leftAligned", true);
        b->setFlat(true);
        b->setCheckable(true);
        b->setChecked(m_route.kind == kind);
        const QString icon = kind == "overview" ? "file-text" : kind == "library" ? "layers" : "folder";
        b->setIcon(Theme::coloredIcon(icon, QColor(Theme::textSecondary()), 15));
        b->setIconSize(QSize(15, 15));
        m_navigationBody->addWidget(b);
        return b;
    };
    nav("Overview", "overview", [this] { showOverview(); });
    nav(QString("All articles   %1").arg(m_library.articles().size()), "library", [this] { showLibrary(); });
    m_savedButton = nav(QString("Saved articles   %1").arg(m_saved.size()), "saved", [this] { showLibrary(true); });
    m_navigationBody->addSpacing(18);
    m_navigationBody->addWidget(text("BROWSE TOPICS", "docsEyebrow"));
    m_topicScroll = new QScrollArea;
    m_topicScroll->setFrameShape(QFrame::NoFrame);
    m_topicScroll->setWidgetResizable(true);
    m_topicScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    auto *topics = new QWidget;
    auto *list = new QVBoxLayout(topics);
    list->setContentsMargins(0, 7, 0, 0);
    list->setSpacing(3);
    for (const auto &group : m_library.groups()) {
        auto *title = action((m_collapsed.contains(group) ? "›  " : "⌄  ") + group,
                             [this, group] {
                                 if (m_collapsed.contains(group))
                                     m_collapsed.remove(group);
                                 else
                                     m_collapsed.insert(group);
                                 QSettings().setValue("docs/collapsedSubjects",
                                                      QStringList(m_collapsed.begin(), m_collapsed.end()));
                                 renderNavigation();
                             },
                             "docsSubject");
        title->setProperty("leftAligned", true);
        title->setFlat(true);
        title->setToolTip(group);
        title->setAccessibleName("Toggle " + group);
        list->addWidget(title);
        m_groupButtons[group] = title;
        if (m_collapsed.contains(group))
            continue;
        for (const auto &article : m_library.articles())
            if (article.group == group) {
                auto *b = action(article.shortTitle, [this, id = article.id] { openArticle(id); }, "docsArticleNav");
                b->setProperty("articleId", article.id);
                b->setProperty("leftAligned", true);
                b->setFlat(true);
                b->setCheckable(true);
                b->setChecked(m_route.kind == "article" && m_route.id == article.id);
                b->setToolTip(article.title);
                list->addWidget(b);
            }
        list->addSpacing(9);
    }
    list->addStretch();
    m_topicScroll->setWidget(topics);
    m_navigationBody->addWidget(m_topicScroll, 1);
    rule(m_navigationBody);
    m_navigationBody->addSpacing(8);
    m_navigationBody->addWidget(text("A reference to return to.", "docsSmallTitle"));
    m_navigationBody->addWidget(text("Concepts, examples and sources.", "docsHint"));
    auto *sources = action("Sources & offline access ↗", [this] { showSources(); }, "docsSources");
    sources->setFlat(true);
    sources->setProperty("leftAligned", true);
    m_navigationBody->addWidget(sources);
    QTimer::singleShot(0, m_topicScroll, [this, previous] { m_topicScroll->verticalScrollBar()->setValue(previous); });
}
void DocsPanel::render(bool keepOffset, const QString &focusObject) {
    const int offset = m_scroll->verticalScrollBar()->value();
    const int maximum = m_scroll->verticalScrollBar()->maximum();
    const double ratio = maximum ? double(offset) / maximum : 0.;
    m_restoring = true;
    const int ticket = ++m_generation;
    m_sections.clear();
    m_sectionLinks.clear();
    m_activeSection.clear();
    m_exampleTabs.clear();
    m_exampleText.clear();
    m_exampleFile.clear();
    m_topicFilter = nullptr;
    m_typeFilter = nullptr;
    if (m_readerTools) {
        m_readerTools->hide();
        delete m_readerTools;
        m_readerTools = nullptr;
    }
    if (m_toc) {
        m_toc->hide();
        delete m_toc;
        m_toc = nullptr;
    }
    if (auto *old = m_scroll->takeWidget()) {
        old->hide();
        old->deleteLater();
    }
    m_content = new QWidget;
    m_content->setObjectName("docsPage");
    m_content->setMinimumWidth(0);
    auto *body = new QVBoxLayout(m_content);
    body->setContentsMargins(m_compact ? 22 : 38, 30, m_compact ? 22 : 38, 36);
    body->setSpacing(20);
    updateResponsive();
    renderNavigation();
    {
        QSignalBlocker block(m_search);
        m_search->setText(m_route.kind == "search" ? m_route.query : QString());
    }
    m_back->setEnabled(m_historyIndex > 0);
    m_forward->setEnabled(m_historyIndex + 1 < m_history.size());
    setStatus({});
    if (!m_library.error().isEmpty()) {
        body->addWidget(text("The reference library is unavailable.", "docsHeading"));
        body->addWidget(text(m_library.error()));
    } else if (m_route.kind == "article")
        renderReader(body);
    else if (m_route.kind == "search")
        renderSearch(body);
    else if (m_route.kind == "library" || m_route.kind == "saved")
        renderCollection(body);
    else
        renderOverview(body);
    body->addStretch();
    m_scroll->setWidget(m_content);
    m_layoutClass = layoutClass();
    if (!keepOffset && m_route.kind != "search") {
        auto *effect = new QGraphicsOpacityEffect(m_content);
        m_content->setGraphicsEffect(effect);
        auto *motion = new MotionValue(effect, [effect](qreal value) { effect->setOpacity(value); });
        motion->snap(.92);
        motion->to(1., true, Theme::motionFastMs());
    }
    // Let the rich-text documents and height-for-width layouts settle before
    // restoring.
    QTimer::singleShot(0, this, [this, ticket, keepOffset, offset, ratio, focusObject] {
        if (ticket != m_generation)
            return;
        m_content->layout()->activate();
        QTimer::singleShot(0, this, [this, ticket, keepOffset, offset, ratio, focusObject] {
            if (ticket != m_generation)
                return;
            auto *bar = m_scroll->verticalScrollBar();
            if (keepOffset)
                bar->setValue(qRound(ratio * bar->maximum()));
            else if (m_route.kind == "article" && !m_route.section.isEmpty())
                scrollToSection(m_route.section, false);
            else if (m_route.kind == "article") {
                const auto position = QSettings().value("docs/positions/" + m_route.id).toMap();
                const bool same = position.value("width").toInt() == m_scroll->viewport()->width() &&
                                  position.value("large").toBool() == m_large;
                bar->setValue(same ? position.value("offset").toInt()
                                   : qRound(position.value("ratio").toDouble() * bar->maximum()));
            } else
                bar->setValue(0);
            Q_UNUSED(offset);
            m_restoring = false;
            updateContents();
            if (!focusObject.isEmpty())
                if (auto *target = findChild<QWidget *>(focusObject))
                    target->setFocus(Qt::OtherFocusReason);
        });
    });
}
DocsCard *DocsPanel::articleCard(const DocsArticle &article, bool libraryCard) {
    auto *card = new DocsCard;
    card->setProperty("articleId", article.id);
    card->setAccessibleName(article.title);
    card->setAccessibleDescription(article.description);
    auto *row = new QHBoxLayout(card);
    row->setContentsMargins(18, 20, 18, 20);
    row->setSpacing(12);
    row->addWidget(symbol(article.icon), 0, Qt::AlignTop);
    auto *detail = new QVBoxLayout;
    detail->setSpacing(9);
    detail->addWidget(text(libraryCard ? article.type : article.group, "docsHint"));
    detail->addWidget(text(article.title, "docsCardTitle"));
    detail->addWidget(text(article.description, "docsCardDescription"));
    detail->addStretch();
    row->addLayout(detail, 1);
    row->addWidget(text("↗", "docsHint"), 0, Qt::AlignTop);
    transparentToMouse(card);
    connect(card, &QPushButton::clicked, this, [this, id = article.id] { openArticle(id); });
    return card;
}
void DocsPanel::renderOverview(QVBoxLayout *body) {
    auto *topline = new QHBoxLayout;
    auto *eyebrow = text("THE MAGNITE REFERENCE", "docsEyebrow");
    eyebrow->setWordWrap(false);
    topline->addWidget(eyebrow);
    topline->addStretch();
    auto *offline = text("●  Local articles · Primary sources", "docsOffline");
    offline->setWordWrap(false);
    topline->addWidget(offline);
    body->addLayout(topline);
    body->addWidget(text("Understand the model.\nUnderstand your data.", "docsHero"));
    body->addWidget(text("Explore what models learn, how examples shape their behavior, and "
                         "what your training results actually mean.",
                         "docsIntro"));
    if (const auto *recent = m_library.article(m_lastArticle)) {
        auto *b = action(
            "Recently opened: " + recent->title + " →", [this, id = recent->id] { openArticle(id); }, "docsRecent");
        b->setFlat(true);
        b->setProperty("leftAligned", true);
        body->addWidget(b);
    }
    auto *overview = new QHBoxLayout;
    overview->setSpacing(24);
    auto *primary = new QVBoxLayout;
    primary->setSpacing(22);
    auto *feature = frame("docsFeature");
    auto *featureRow = new QHBoxLayout(feature);
    featureRow->setContentsMargins(24, 26, 24, 26);
    featureRow->setSpacing(22);
    auto *featureLayout = new QVBoxLayout;
    featureLayout->setSpacing(18);
    featureLayout->setAlignment(Qt::AlignTop);
    featureLayout->addWidget(text("UNDERSTANDING DATA", "docsEyebrow"));
    auto *featureTitle = text("The data behind the\nbehavior.", "docsFeatureTitle");
    featureTitle->ensurePolished();
    featureTitle->setMinimumHeight(featureTitle->fontMetrics().lineSpacing() * 2 + 4);
    featureLayout->addWidget(featureTitle);
    featureLayout->addWidget(text("Valid rows are only the beginning. Look at the examples, "
                                  "repetitions and missing cases that make up the learning signal.",
                                  "docsCardDescription"));
    featureLayout->addWidget(
        action("Explore dataset quality →", [this] { openArticle("data-quality"); }, "primaryBtn"), 0, Qt::AlignLeft);
    featureRow->addLayout(featureLayout, 2);
    if (mainWidth() >= 650) {
        auto *diagram = new DocsDiagram(DocsDiagram::Data);
        diagram->setFixedWidth(165);
        diagram->setFixedHeight(245);
        featureRow->addWidget(diagram);
    }
    primary->addWidget(feature);
    auto *concepts = new QHBoxLayout;
    concepts->addWidget(text("Concepts to explore", "docsSectionTitle"));
    concepts->addStretch();
    auto *all = action("All articles →", [this] { showLibrary(); }, "docsTextButton");
    all->setFlat(true);
    concepts->addWidget(all);
    primary->addLayout(concepts);
    auto *grid = new QGridLayout;
    grid->setSpacing(12);
    const QStringList featured{"model-learning",    "tokens-context", "data-quality",
                               "data-distribution", "generalization", "lora"};
    const int columns = mainWidth() >= 650 ? 2 : 1;
    for (int i = 0; i < featured.size(); ++i)
        if (const auto *a = m_library.article(featured[i]))
            grid->addWidget(articleCard(*a), i / columns, i % columns);
    for (int c = 0; c < columns; ++c)
        grid->setColumnStretch(c, 1);
    primary->addLayout(grid);
    overview->addLayout(primary, 3);
    auto *questions = frame("docsQuestions");
    auto *q = new QVBoxLayout(questions);
    q->setContentsMargins(mainWidth() >= 860 ? 24 : 0, 8, 0, 8);
    q->setSpacing(16);
    q->addWidget(text("QUESTIONS WORTH ASKING", "docsEyebrow"));
    q->addWidget(text("Follow a question.", "docsSectionTitle"));
    q->addWidget(text("Open the explanation that connects to what you’re working on.", "docsCardDescription"));
    const QList<QStringList> prompts{{"What is the model actually learning?", "model-learning", "objective"},
                                     {"Is my validation data really unseen?", "data-distribution", "split"},
                                     {"What does a lower loss tell me?", "generalization", "score"},
                                     {"What happens to long examples?", "tokens-context", "limits"},
                                     {"How does retrieval differ from training?", "rag", "retrieval"}};
    for (const auto &p : prompts) {
        auto *card = new DocsCard;
        card->setFlat(true);
        auto *row = new QHBoxLayout(card);
        row->setContentsMargins(0, 14, 0, 14);
        row->addWidget(text(p[0], "docsQuestion"), 1);
        row->addWidget(text("↗", "docsHint"));
        transparentToMouse(card);
        connect(card, &QPushButton::clicked, this, [this, p] { openArticle(p[1], p[2]); });
        q->addWidget(card);
        rule(q);
    }
    q->addWidget(text("Articles connect concepts, examples and app behavior. "
                      "Explore them in any order.",
                      "docsHint"));
    q->addStretch();
    if (mainWidth() >= 860) {
        questions->setMinimumWidth(220);
        questions->setMaximumWidth(280);
        overview->addWidget(questions, 1);
        body->addLayout(overview);
    } else {
        body->addLayout(overview);
        body->addWidget(questions);
    }
    rule(body);
    body->addWidget(text("Browse by subject", "docsSectionTitle"));
    auto *subjects = new QGridLayout;
    subjects->setSpacing(12);
    int i = 0;
    const int count = mainWidth() >= 700 ? 3 : mainWidth() >= 500 ? 2 : 1;
    const QStringList descriptions{"Prediction, tokens and the artifacts you work with.",
                                   "Coverage, formatting and the patterns in your examples.",
                                   "Training controls and interpreting what a run tells you.",
                                   "LoRA, retrieval and working with adapters.",
                                   "Local workflows, troubleshooting and terminology."};
    for (const auto &group : m_library.groups()) {
        auto *card = new DocsCard;
        auto *l = new QVBoxLayout(card);
        l->setContentsMargins(18, 18, 18, 18);
        l->addWidget(text(group, "docsCardTitle"));
        l->addWidget(text(descriptions.value(i), "docsCardDescription"));
        int n = 0;
        for (const auto &a : m_library.articles())
            if (a.group == group)
                ++n;
        l->addWidget(text(QString("%1 articles   →").arg(n), "docsEyebrow"));
        transparentToMouse(card);
        connect(card, &QPushButton::clicked, this, [this, group] {
            Route r;
            r.kind = "library";
            r.topic = group;
            navigate(r);
        });
        subjects->addWidget(card, i / count, i % count);
        ++i;
    }
    for (int c = 0; c < count; ++c)
        subjects->setColumnStretch(c, 1);
    body->addLayout(subjects);
    body->addWidget(action(
        "Keep the context, and the sources.   Sources & offline access ↗", [this] { showSources(); },
        "docsSourcesBanner"));
}
void DocsPanel::renderCollection(QVBoxLayout *body) {
    const bool saved = m_route.kind == "saved";
    body->addWidget(text("YOUR REFERENCE SHELF  ·  AVAILABLE OFFLINE", "docsEyebrow"));
    body->addWidget(text(saved ? "Saved articles." : "Explore the reference.", "docsHeading"));
    body->addWidget(text(saved ? "Explanations and examples you want to return to."
                               : "Read about the concepts, data and decisions behind your "
                                 "work. Every article stands on its own.",
                         "docsIntro"));
    auto *filters = new QHBoxLayout;
    filters->setSpacing(12);
    m_topicFilter = new ComboBox;
    m_topicFilter->setObjectName("docsTopicFilter");
    m_topicFilter->setAccessibleName("Filter articles by subject");
    m_topicFilter->addItem("All subjects", "");
    for (const auto &group : m_library.groups())
        m_topicFilter->addItem(group, group);
    m_topicFilter->setCurrentIndex(qMax(0, m_topicFilter->findData(m_route.topic)));
    m_typeFilter = new ComboBox;
    m_typeFilter->setObjectName("docsTypeFilter");
    m_typeFilter->setAccessibleName("Filter articles by format");
    m_typeFilter->addItem("All formats", "");
    for (const auto &type : m_library.types())
        m_typeFilter->addItem(type, type);
    m_typeFilter->setCurrentIndex(qMax(0, m_typeFilter->findData(m_route.type)));
    filters->addWidget(m_topicFilter, 2);
    filters->addWidget(m_typeFilter, 1);
    body->addLayout(filters);
    connect(m_topicFilter, qOverload<int>(&QComboBox::activated), this, [this] {
        auto route = m_route;
        route.topic = m_topicFilter->currentData().toString();
        navigate(route);
    });
    connect(m_typeFilter, qOverload<int>(&QComboBox::activated), this, [this] {
        auto route = m_route;
        route.type = m_typeFilter->currentData().toString();
        navigate(route);
    });
    auto *grid = new QGridLayout;
    grid->setSpacing(14);
    int i = 0;
    const int columns = mainWidth() >= 650 ? 2 : 1;
    for (const auto &article : m_library.articles())
        if ((!saved || m_saved.contains(article.id)) && (m_route.topic.isEmpty() || m_route.topic == article.group) &&
            (m_route.type.isEmpty() || m_route.type == article.type)) {
            auto *host = new QWidget;
            auto *l = new QVBoxLayout(host);
            l->setContentsMargins(0, 0, 0, 0);
            l->setSpacing(3);
            l->addWidget(articleCard(article, true), 1);
            auto *bookmark = action(
                m_saved.contains(article.id) ? "Saved article ✓" : "Save article",
                [this, id = article.id] { toggleSaved(id); }, "docsBookmark");
            bookmark->setProperty("articleId", article.id);
            bookmark->setCheckable(true);
            bookmark->setChecked(m_saved.contains(article.id));
            bookmark->setAccessibleName((m_saved.contains(article.id) ? "Unsave " : "Save ") + article.title);
            bookmark->setFlat(true);
            l->addWidget(bookmark, 0, Qt::AlignLeft);
            grid->addWidget(host, i / columns, i % columns);
            ++i;
        }
    body->addWidget(text(QString("%1 %2").arg(i).arg(i == 1 ? "article" : "articles"), "docsHint"));
    if (i) {
        for (int c = 0; c < columns; ++c)
            grid->setColumnStretch(c, 1);
        body->addLayout(grid);
    } else {
        delete grid;
        body->addWidget(text(saved && m_route.topic.isEmpty() && m_route.type.isEmpty()
                                 ? "Keep useful references nearby."
                                 : "No articles match these filters.",
                             "docsSectionTitle"));
        body->addWidget(text(saved ? "Save an article to find it here whenever you need it."
                                   : "Choose another subject or clear the filters."));
        body->addWidget(action("Browse all articles →", [this] { showLibrary(); }, "primaryBtn"), 0, Qt::AlignLeft);
    }
}
void DocsPanel::renderSearch(QVBoxLayout *body) {
    const auto results = m_library.search(m_route.query);
    const auto words = DocsLibrary::terms(m_route.query);
    body->addWidget(text("SEARCH THE LOCAL LIBRARY", "docsEyebrow"));
    body->addWidget(text(QString("%1 %2 for “%3”")
                             .arg(results.size())
                             .arg(results.size() == 1 ? "article" : "articles", m_route.query.trimmed()),
                         "docsHeading"));
    body->addWidget(text("Search matches article titles, concepts and sections. "
                         "Results stay on this device.",
                         "docsIntro"));
    auto *suggestions = new QHBoxLayout;
    for (const auto &word : QStringList{"memory", "JSONL", "validation", "LoRA"}) {
        auto *b = action(word, [this, word] { showSearch(word); }, "docsSuggestion");
        suggestions->addWidget(b);
    }
    suggestions->addStretch();
    body->addLayout(suggestions);
    for (const auto &match : results) {
        const auto *article = m_library.article(match.articleId);
        auto *card = new DocsCard;
        card->setAccessibleName(article->title + " — " + match.heading);
        auto *l = new QVBoxLayout(card);
        l->setContentsMargins(22, 20, 22, 20);
        l->setSpacing(9);
        l->addWidget(text(article->group + "   /   " + article->type, "docsEyebrow"));
        l->addWidget(rich(DocsLibrary::highlight(article->title, words) + " ↗", "docsCardTitle"));
        l->addWidget(rich(DocsLibrary::highlight(match.snippet, words), "docsBody"));
        if (!match.sectionId.isEmpty())
            l->addWidget(rich("In this article: " + DocsLibrary::highlight(match.heading, words) + " →", "docsHint"));
        transparentToMouse(card);
        connect(card, &QPushButton::clicked, this, [this, match] { openArticle(match.articleId, match.sectionId); });
        body->addWidget(card);
    }
    if (results.isEmpty()) {
        body->addWidget(text("Try a broader term.", "docsSectionTitle"));
        body->addWidget(text("You can search for “dataset”, “memory”, “adapter” or “loss”."));
        body->addWidget(action("Browse the library →", [this] { showLibrary(); }, "primaryBtn"), 0, Qt::AlignLeft);
    }
}
void DocsPanel::renderReader(QVBoxLayout *body) {
    const auto *article = m_library.article(m_route.id);
    if (!article)
        return;
    m_readerTools = frame("docsReaderTools");
    auto *tools = new QHBoxLayout(m_readerTools);
    tools->setContentsMargins(22, 8, 22, 8);
    tools->setSpacing(12);
    auto *back = action("← Reference", [this] { showOverview(); }, "docsTextButton");
    back->setFlat(true);
    tools->addWidget(back);
    tools->addWidget(text(article->shortTitle, "docsHint"), 1);
    auto *size = action(
        "Aa",
        [this] {
            savePosition();
            m_large = !m_large;
            QSettings().setValue("docs/textSize", m_large ? "large" : "standard");
            render(true, "docsTextSize");
        },
        "docsTextSize");
    size->setCheckable(true);
    size->setChecked(m_large);
    size->setAccessibleName(m_large ? "Use standard text size" : "Use larger text size");
    tools->addWidget(size);
    auto *focus = action(m_focus ? "Show navigation" : "Focus mode", [this] { setFocusMode(!m_focus); }, "docsFocus");
    focus->setCheckable(true);
    focus->setChecked(m_focus);
    tools->addWidget(focus);
    contentLayout()->insertWidget(1, m_readerTools);
    auto *crumbs = new QHBoxLayout;
    auto *all = action("All articles", [this] { showLibrary(); }, "docsTextButton");
    all->setFlat(true);
    crumbs->addWidget(all);
    crumbs->addWidget(text("/", "docsHint"));
    auto *group = action(
        article->group,
        [this, topic = article->group] {
            Route r;
            r.kind = "library";
            r.topic = topic;
            navigate(r);
        },
        "docsTextButton");
    group->setFlat(true);
    crumbs->addWidget(group);
    crumbs->addStretch();
    body->addLayout(crumbs);
    body->addWidget(text(article->type + "   ·   Local reference", "docsEyebrow"));
    body->addWidget(text(article->title, "docsHeading"));
    body->addWidget(text(article->description, "docsIntro"));
    auto *actions = new QHBoxLayout;
    auto *save = action(
        m_saved.contains(article->id) ? "Saved article ✓" : "Save article",
        [this, id = article->id] { toggleSaved(id); }, "docsSaveArticle");
    save->setCheckable(true);
    save->setChecked(m_saved.contains(article->id));
    actions->addWidget(save);
    auto *top = action("↑ Back to top", [this] { m_scroll->verticalScrollBar()->setValue(0); }, "docsTextButton");
    top->setFlat(true);
    actions->addWidget(top);
    actions->addStretch();
    body->addLayout(actions);
    rule(body);
    if (mainWidth() >= 860) {
        m_toc = new QWidget;
        m_toc->setObjectName("docsContents");
        m_toc->setFixedWidth(190);
        auto *contents = new QVBoxLayout(m_toc);
        contents->setContentsMargins(12, 32, 18, 16);
        contents->setSpacing(8);
        contents->addWidget(text("IN THIS ARTICLE", "docsEyebrow"));
        for (const auto &section : article->sections) {
            auto *b = action(section.title, [this, id = section.id] { scrollToSection(id); }, "docsSectionLink");
            b->setFlat(true);
            b->setProperty("leftAligned", true);
            b->setCheckable(true);
            b->setToolTip(section.title);
            QStringList lines;
            QString line;
            for (const auto &word : section.title.split(' ')) {
                if (!line.isEmpty() && line.size() + word.size() > 23) {
                    lines << line;
                    line.clear();
                }
                line += (line.isEmpty() ? "" : " ") + word;
            }
            if (!line.isEmpty())
                lines << line;
            b->setText(lines.join('\n'));
            contents->addWidget(b);
            m_sectionLinks[section.id] = b;
        }
        contents->addSpacing(20);
        contents->addWidget(text("Local reference.\nSources below.", "docsHint"));
        contents->addStretch();
        m_shellLayout->addWidget(m_toc);
    } else {
        auto *toc = frame("docsInlineContents");
        auto *contents = new QVBoxLayout(toc);
        contents->setContentsMargins(16, 14, 16, 14);
        contents->setSpacing(4);
        auto *heading = action(
            QString("In this article   ·   %1 sections   ⌄").arg(article->sections.size()), [] {},
            "docsContentsToggle");
        heading->setFlat(true);
        heading->setCheckable(true);
        contents->addWidget(heading);
        auto *links = new QWidget;
        auto *ll = new QVBoxLayout(links);
        ll->setContentsMargins(0, 0, 0, 0);
        ll->setSpacing(2);
        for (const auto &section : article->sections) {
            auto *b = action(section.title, [this, id = section.id] { scrollToSection(id); }, "docsSectionLink");
            b->setFlat(true);
            b->setProperty("leftAligned", true);
            b->setCheckable(true);
            ll->addWidget(b);
            m_sectionLinks[section.id] = b;
        }
        contents->addWidget(links);
        links->hide();
        connect(heading, &QPushButton::toggled, links, &QWidget::setVisible);
        body->addWidget(toc);
    }
    for (const auto &section : article->sections) {
        auto *host = new QWidget;
        host->setObjectName("docsSection");
        host->setProperty("sectionId", section.id);
        host->setFocusPolicy(Qt::StrongFocus);
        auto *sectionBody = new QVBoxLayout(host);
        sectionBody->setContentsMargins(0, 12, 0, 8);
        sectionBody->setSpacing(16);
        auto *title = new QHBoxLayout;
        title->addWidget(text(section.title, "docsSectionTitle"), 1);
        auto *permalink = action(
            "#",
            [this, id = article->id, section = section.id] {
                QApplication::clipboard()->setText(DocsLibrary::articleLink(id, section).toString());
                setStatus("Section link copied. Paste it into Docs search to open it.");
            },
            "docsPermalink");
        permalink->setAccessibleName("Copy link to " + section.title);
        permalink->setToolTip("Copy section link");
        permalink->setFlat(true);
        title->addWidget(permalink);
        sectionBody->addLayout(title);
        addArticleBody(sectionBody, section.html);
        body->addWidget(host);
        m_sections[section.id] = host;
    }
    rule(body);
    body->addWidget(text("SOURCES & FURTHER READING", "docsEyebrow"));
    body->addWidget(text("Concepts are linked to primary references. Magnite-specific "
                         "behavior is checked against the local app documentation.",
                         "docsCardDescription"));
    for (const auto &id : article->sources)
        if (const auto *source = m_library.source(id)) {
            auto *b = action(
                source->label + "   ·   " + (source->local ? "Local reference ↗" : "Web reference ↗"),
                [this, id] { openSource(id); }, "docsSourceLink");
            b->setProperty("sourceId", id);
            b->setProperty("leftAligned", true);
            b->setFlat(true);
            b->setToolTip(source->local ? "Read bundled reference" : "Open official reference in your browser");
            body->addWidget(b);
        }
    rule(body);
    body->addWidget(text("Related reading", "docsSectionTitle"));
    body->addWidget(text("Other explanations connected to this subject.", "docsHint"));
    auto *related = new QGridLayout;
    related->setSpacing(12);
    int i = 0;
    const int columns = mainWidth() >= 860 ? 2 : 1;
    for (const auto &id : article->related)
        if (const auto *a = m_library.article(id)) {
            related->addWidget(articleCard(*a), i / columns, i % columns);
            ++i;
        }
    for (int c = 0; c < columns; ++c)
        related->setColumnStretch(c, 1);
    body->addLayout(related);
}
void DocsPanel::addArticleBody(QVBoxLayout *body, const QString &html) {
    QRegularExpression special("<(div|figure) "
                               "(?:class=\"(concept-flow|flow-diagram|article-callout|distribution-"
                               "example)\"|id=\"dataset-example\")>");
    int cursor = 0;
    while (cursor < html.size()) {
        const auto match = special.match(html, cursor);
        const int end = match.hasMatch() ? match.capturedStart() : html.size();
        if (end > cursor) {
            auto *view = new DocsTextView;
            view->setContent(html.mid(cursor, end - cursor), m_large);
            connect(view, &QTextBrowser::anchorClicked, this, &DocsPanel::handleTextLink);
            body->addWidget(view);
        }
        if (!match.hasMatch())
            break;
        const int close = matchingClose(html, end, match.captured(1));
        const auto part = html.mid(end, close - end);
        const auto kind = match.captured(2);
        if (part.startsWith("<div id="))
            addExample(body);
        else if (kind == "article-callout") {
            auto *box = frame("docsCallout");
            auto *l = new QVBoxLayout(box);
            l->setContentsMargins(17, 14, 17, 14);
            auto *view = new DocsTextView;
            // Qt rich text does not implement the preview's display:block on
            // <strong>. Use paragraphs and leave padding to the native frame.
            const int start = part.indexOf('>') + 1;
            QString content = part.mid(start, part.lastIndexOf("</div>") - start);
            content.replace("</strong>", "</strong></p><p>");
            view->setContent("<p>" + content + "</p>", m_large);
            l->addWidget(view);
            body->addWidget(box);
        } else
            body->addWidget(new DocsDiagram(kind == "distribution-example" ? DocsDiagram::Distribution
                                            : kind == "flow-diagram"       ? DocsDiagram::LoRA
                                            : part.contains("Tokenizer", Qt::CaseInsensitive) ? DocsDiagram::Tokens
                                                                                              : DocsDiagram::Learning));
        cursor = close;
    }
}
void DocsPanel::addExample(QVBoxLayout *body) {
    auto *box = frame("docsExample");
    auto *layout = new QVBoxLayout(box);
    layout->setContentsMargins(14, 14, 14, 14);
    layout->setSpacing(12);
    auto *tabs = new QHBoxLayout;
    for (const auto &id : QStringList{"jsonl", "messages", "csv"})
        if (const auto *example = m_library.example(id)) {
            auto *b = action(
                example->name,
                [this, id] {
                    m_example = id;
                    updateExample();
                },
                "docsExampleTab");
            b->setProperty("format", id);
            b->setCheckable(true);
            tabs->addWidget(b);
            m_exampleTabs[id] = b;
        }
    tabs->addStretch();
    m_copyExample = action(
        "Copy",
        [this] {
            if (const auto *e = m_library.example(m_example)) {
                QApplication::clipboard()->setText(e->content);
                m_copyExample->setText("Copied ✓");
                QPointer<QPushButton> button = m_copyExample;
                QTimer::singleShot(1600, this, [button] {
                    if (button)
                        button->setText("Copy");
                });
            }
        },
        "docsCopyExample");
    tabs->addWidget(m_copyExample);
    layout->addLayout(tabs);
    auto *file = new QHBoxLayout;
    m_exampleFile = text("", "docsHint");
    m_exampleFile->setWordWrap(false);
    file->addWidget(m_exampleFile, 1);
    m_wrapExample = action(
        "Wrap lines",
        [this] {
            m_wrap = !m_wrap;
            updateExample();
        },
        "docsWrapExample");
    m_wrapExample->setCheckable(true);
    file->addWidget(m_wrapExample);
    layout->addLayout(file);
    m_exampleText = new QPlainTextEdit;
    m_exampleText->setObjectName("docsExampleCode");
    m_exampleText->setReadOnly(true);
    m_exampleText->setMinimumWidth(0);
    m_exampleText->setFixedHeight(210);
    layout->addWidget(m_exampleText);
    auto *footer = new QHBoxLayout;
    auto *records = text("UTF-8 · 2 sample records", "docsHint");
    records->setWordWrap(false);
    footer->addWidget(records);
    footer->addStretch();
    footer->addWidget(action("Download example ↓", [this] { downloadExample(); }, "docsDownloadExample"));
    layout->addLayout(footer);
    body->addWidget(box);
    updateExample();
}
void DocsPanel::updateExample() {
    const auto *example = m_library.example(m_example);
    if (!example || !m_exampleText)
        return;
    m_exampleText->setPlainText(example->content);
    m_exampleText->setAccessibleName(example->name + " dataset example");
    m_exampleText->setLineWrapMode(m_wrap ? QPlainTextEdit::WidgetWidth : QPlainTextEdit::NoWrap);
    m_exampleFile->setText(example->filename);
    m_wrapExample->setChecked(m_wrap);
    for (auto it = m_exampleTabs.begin(); it != m_exampleTabs.end(); ++it)
        if (it.value())
            it.value()->setChecked(it.key() == m_example);
    m_copyExample->setText("Copy");
}
void DocsPanel::downloadExample() {
    const auto *example = m_library.example(m_example);
    if (!example)
        return;
    const QString destination = chooseExampleDestination(example->filename);
    if (destination.isEmpty())
        return;
    QSaveFile file(destination);
    const auto bytes = example->content.toUtf8();
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        setStatus("Could not save the example: " + file.errorString());
        return;
    }
    setStatus("Example saved to " + destination);
}
void DocsPanel::toggleSaved(const QString &id) {
    if (!m_library.article(id))
        return;
    if (m_saved.contains(id))
        m_saved.remove(id);
    else
        m_saved.insert(id);
    QSettings().setValue("docs/savedArticles", QStringList(m_saved.begin(), m_saved.end()));
    if (m_route.kind == "saved") {
        savePosition();
        render(true);
        return;
    }
    if (m_savedButton)
        m_savedButton->setText(QString("Saved articles   %1").arg(m_saved.size()));
    for (auto *b : findChildren<QPushButton *>())
        if ((b->objectName() == "docsSaveArticle" && m_route.id == id) ||
            (b->objectName() == "docsBookmark" && b->property("articleId").toString() == id)) {
            b->setChecked(m_saved.contains(id));
            b->setText(m_saved.contains(id) ? "Saved article ✓" : "Save article");
            b->setAccessibleName((m_saved.contains(id) ? "Unsave " : "Save ") + m_library.article(id)->title);
        }
}
void DocsPanel::updateContents() {
    const auto *article = m_library.article(currentArticleId());
    if (!article)
        return;
    const int threshold = m_scroll->verticalScrollBar()->value() + 40;
    QString active = article->sections.first().id;
    for (const auto &section : article->sections)
        if (m_sections.value(section.id) && m_sections[section.id]->mapTo(m_content, QPoint()).y() <= threshold)
            active = section.id;
    m_activeSection = active;
    for (auto it = m_sectionLinks.begin(); it != m_sectionLinks.end(); ++it)
        if (it.value())
            it.value()->setChecked(it.key() == active);
}
void DocsPanel::scrollToSection(const QString &section, bool focus) {
    const auto widget = m_sections.value(section);
    if (!widget)
        return;
    m_scroll->verticalScrollBar()->setValue(widget->mapTo(m_content, QPoint()).y() - 16);
    updateContents();
    if (focus)
        widget->setFocus(Qt::OtherFocusReason);
}
void DocsPanel::showSources() {
    SettingsDetailDialog dialog(this);
    dialog.setWindowTitle("Sources & offline access");
    dialog.resize(qMin(700, qMax(320, width() - 40)), qMin(700, qMax(420, height() - 50)));
    auto *root = new QVBoxLayout(&dialog);
    root->setContentsMargins(26, 24, 26, 24);
    root->setSpacing(18);
    auto *header = new QHBoxLayout;
    header->addWidget(text("BUILT FOR LOCAL WORK", "docsEyebrow"), 1);
    auto *close = new Button("×");
    close->setAccessibleName("Close sources");
    close->setFixedWidth(32);
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    header->addWidget(close);
    root->addLayout(header);
    root->addWidget(text("A small library. Always with you.", "docsSectionTitle"));
    root->addWidget(text("Core articles are available locally. External "
                         "references open only when you choose them."));
    auto *view = new DocsTextView(nullptr, false);
    QString html = "<h3>Local articles</h3><p>Original explanations of ML concepts and "
                   "Magnite behavior, with examples and small diagrams. Reviewed alongside "
                   "the app.</p><h3>Primary references</h3><p>Links to official ML, data "
                   "and model documentation. Internet is needed only to open these external "
                   "pages.</p><h3>What already exists locally</h3>";
    for (const auto &id : m_library.sourceIds())
        if (const auto *source = m_library.source(id); source && source->local)
            html += "<p><a href=\"magnite-source:" + id + "\">" + source->label.toHtmlEscaped() + " ↗</a></p>";
    html += "<p>16 reference articles are bundled with the app. Docs does not "
            "download documentation in the background or connect to an AI "
            "service. Saved articles and reading positions stay in this app’s "
            "local preferences.</p>";
    view->setContent(html);
    connect(view, &QTextBrowser::anchorClicked, &dialog, [this](const QUrl &url) {
        if (url.scheme() == "magnite-source")
            openSource(url.path());
    });
    root->addWidget(view, 1);
    dialog.exec();
}
void DocsPanel::openSource(const QString &id) {
    const auto *source = m_library.source(id);
    if (!source)
        return;
    if (!source->local) {
        if (!openExternalUrl(QUrl(source->location)))
            setStatus("Could not open the reference in your browser.");
        return;
    }
    QFile file(":/docs/" + source->location);
    if (!file.open(QIODevice::ReadOnly)) {
        setStatus("The bundled reference could not be opened.");
        return;
    }
    SettingsDetailDialog dialog(this);
    dialog.setWindowTitle(source->label);
    dialog.resize(qMin(800, qMax(320, width() - 30)), qMin(760, qMax(420, height() - 30)));
    auto *layout = new QVBoxLayout(&dialog);
    layout->setContentsMargins(24, 24, 24, 24);
    auto *header = new QHBoxLayout;
    header->addWidget(text(source->label, "docsSectionTitle"), 1);
    auto *close = new Button("×");
    close->setAccessibleName("Close reference");
    close->setFixedWidth(32);
    header->addWidget(close);
    connect(close, &QPushButton::clicked, &dialog, &QDialog::accept);
    layout->addLayout(header);
    auto *view = new DocsTextView(nullptr, false);
    view->setReferenceMarkdown(QString::fromUtf8(file.readAll()));
    auto *notice = text("", "docsHint");
    notice->hide();
    connect(view, &QTextBrowser::anchorClicked, &dialog, [this, view, notice](const QUrl &url) {
        if (url.isRelative() && url.path().isEmpty() && !url.fragment().isEmpty()) {
            view->scrollToAnchor(url.fragment());
            return;
        }
        if (url.scheme() == "https") {
            if (!openExternalUrl(url)) {
                notice->setText("Could not open the reference in your browser.");
                notice->show();
            }
            return;
        }
        if (url.isRelative())
            for (const auto &key : m_library.sourceIds())
                if (const auto *target = m_library.source(key);
                    target && target->local && target->location.section('/', -1) == url.path().section('/', -1)) {
                    openSource(key);
                    return;
                }
        notice->setText("That reference is not bundled. Sources & offline "
                        "access lists the available local documents.");
        notice->show();
    });
    layout->addWidget(view, 1);
    layout->addWidget(notice);
    dialog.exec();
}
void DocsPanel::handleTextLink(const QUrl &url) {
    if (url.scheme() == "magnite-docs") {
        if (!openLink(url))
            setStatus("This article link is unavailable.");
    } else if (url.scheme() == "https" && !url.host().isEmpty()) {
        if (!openExternalUrl(url))
            setStatus("Could not open the reference in your browser.");
    }
}
void DocsPanel::setFocusMode(bool focus) {
    if (m_focus == focus || m_route.kind != "article")
        return;
    savePosition();
    m_focus = focus;
    closeNavigation();
    emit focusModeChanged(focus);
    render(true, "docsFocus");
}
void DocsPanel::toggleNavigation() {
    if (!m_compact)
        return;
    if (m_navOpen) {
        closeNavigation();
        return;
    }
    m_navOpen = true;
    m_scrim->setGeometry(m_shell->rect());
    m_scrim->show();
    m_scrim->raise();
    m_navigation->setGeometry(0, 0, qMin(270, m_shell->width() - 32), m_shell->height());
    m_navigation->show();
    m_navigation->raise();
    m_scroll->setEnabled(false);
    if (m_toc)
        m_toc->setEnabled(false);
    m_menu->setAccessibleName("Close topic navigation");
    if (auto *first = m_navigation->findChild<QPushButton *>())
        first->setFocus();
}
void DocsPanel::closeNavigation() {
    if (!m_navOpen)
        return;
    m_navOpen = false;
    m_scrim->hide();
    m_navigation->setVisible(!m_compact && !m_focus);
    m_scroll->setEnabled(true);
    if (m_toc)
        m_toc->setEnabled(true);
    m_menu->setAccessibleName("Open topic navigation");
    m_menu->setFocus();
}
void DocsPanel::updateResponsive() {
    const bool compact = width() < 760;
    if (compact != m_compact) {
        closeNavigation();
        m_compact = compact;
    }
    m_shellLayout->removeWidget(m_navigation);
    if (!m_compact && !m_focus) {
        m_shellLayout->insertWidget(0, m_navigation);
        m_navigation->setFixedWidth(222);
        m_navigation->show();
    } else
        m_navigation->setVisible(m_navOpen);
    m_menu->setVisible(m_compact && !m_focus);
    m_toolbarSubtitle->setVisible(width() >= 980);
    if (m_navOpen) {
        m_scrim->setGeometry(m_shell->rect());
        m_navigation->setGeometry(0, 0, 222, m_shell->height());
    }
}
void DocsPanel::scheduleRefresh() {
    if (m_refreshPending)
        return;
    m_refreshPending = true;
    QTimer::singleShot(0, this, [this] {
        m_refreshPending = false;
        if (!m_ready)
            return;
        savePosition();
        render(true);
    });
}
void DocsPanel::resizeEvent(QResizeEvent *event) {
    BasePanel::resizeEvent(event);
    if (m_ready)
        m_resizeTimer->start();
}
void DocsPanel::showEvent(QShowEvent *event) {
    BasePanel::showEvent(event);
    if (m_ready) {
        updateResponsive();
        if (m_layoutClass != layoutClass())
            render(true);
    }
}
void DocsPanel::hideEvent(QHideEvent *event) {
    savePosition();
    closeNavigation();
    BasePanel::hideEvent(event);
}
void DocsPanel::changeEvent(QEvent *event) {
    BasePanel::changeEvent(event);
    if (m_ready && (event->type() == QEvent::PaletteChange || event->type() == QEvent::StyleChange ||
                    event->type() == QEvent::FontChange))
        scheduleRefresh();
}
bool DocsPanel::eventFilter(QObject *watched, QEvent *event) {
    if (event->type() != QEvent::KeyPress || !isVisible())
        return BasePanel::eventFilter(watched, event);
    auto *target = qobject_cast<QWidget *>(watched);
    if (!target || !(target == this || isAncestorOf(target)) || target->window() != window())
        return BasePanel::eventFilter(watched, event);
    auto *key = static_cast<QKeyEvent *>(event);
    if (key->key() == Qt::Key_Escape) {
        if (m_navOpen) {
            closeNavigation();
            return true;
        }
        if (m_focus) {
            setFocusMode(false);
            return true;
        }
        if (!m_search->text().isEmpty()) {
            m_search->clear();
            showSearch({});
            return true;
        }
    }
    if (key->key() == Qt::Key_Slash && key->modifiers() == Qt::NoModifier && !qobject_cast<QLineEdit *>(target) &&
        !qobject_cast<QPlainTextEdit *>(target) && !qobject_cast<QTextEdit *>(target)) {
        closeNavigation();
        m_search->setFocus();
        return true;
    }
    if (target->objectName() == "docsExampleTab" && (key->key() == Qt::Key_Right || key->key() == Qt::Key_Left ||
                                                     key->key() == Qt::Key_Home || key->key() == Qt::Key_End)) {
        const QStringList ids{"jsonl", "messages", "csv"};
        int index = ids.indexOf(target->property("format").toString());
        index = key->key() == Qt::Key_Home  ? 0
                : key->key() == Qt::Key_End ? 2
                                            : (index + (key->key() == Qt::Key_Right ? 1 : 2)) % 3;
        m_example = ids[index];
        updateExample();
        if (m_exampleTabs[m_example])
            m_exampleTabs[m_example]->setFocus();
        return true;
    }
    if (m_navOpen && (key->key() == Qt::Key_Tab || key->key() == Qt::Key_Backtab)) {
        QList<QWidget *> items;
        for (auto *b : m_navigation->findChildren<QPushButton *>())
            if (b->isVisible() && b->isEnabled())
                items << b;
        if (!items.isEmpty()) {
            int index = items.indexOf(QApplication::focusWidget());
            index = (index + ((key->modifiers().testFlag(Qt::ShiftModifier) || key->key() == Qt::Key_Backtab)
                                  ? items.size() - 1
                                  : 1)) %
                    items.size();
            items[index]->setFocus(Qt::TabFocusReason);
            return true;
        }
    }
    return BasePanel::eventFilter(watched, event);
}
