#include "panels/blueprinttopbar.h"

#include "textformat.h"
#include "theme.h"
#include "widgets/button.h"

#include <QButtonGroup>
#include <QEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QVBoxLayout>

namespace {
// A single-line label that may shrink: it paints its text elided and shows the whole text as a tooltip.
class ElidedLabel : public QLabel {
public:
    explicit ElidedLabel(const char *name) {
        setObjectName(name);
        setTextFormat(Qt::PlainText);
        setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this);
        painter.setPen(palette().color(foregroundRole()));
        const QRect box = contentsRect();
        const QString shown = fontMetrics().elidedText(text(), Qt::ElideRight, box.width());
        painter.drawText(box, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, shown);
        setToolTip(shown == text() ? QString() : text());
    }
};

QLabel *pill(const QString &text) {
    auto *label = new QLabel(text);
    label->setObjectName("blueprintPill");
    label->setTextFormat(Qt::PlainText);
    label->hide();
    return label;
}

Button *iconButton(const QString &icon, const QString &name) {
    auto *button = new Button;
    button->setFlat(true);
    button->setObjectName("blueprintIconButton");
    button->setAccessibleName(name);
    button->setToolTip(name);
    button->setFixedSize(32, 32);
    button->setProperty("themeIcon", icon);  // Theme re-tints it on a theme change
    button->setIcon(Theme::coloredIcon(icon, QColor(Theme::textSecondary()), 16));
    return button;
}

// The validation status dot: a solid centre in a soft halo of the same colour.
QIcon statusDot(const QColor &colour) {
    QPixmap pixmap(32, 32);
    pixmap.setDevicePixelRatio(2);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(Qt::NoPen);
    QColor halo(colour);
    halo.setAlphaF(0.18);
    painter.setBrush(halo);
    painter.drawEllipse(QPointF(8, 8), 6.5, 6.5);
    painter.setBrush(colour);
    painter.drawEllipse(QPointF(8, 8), 3.5, 3.5);
    return QIcon(pixmap);
}

constexpr int titleRoom = 96;  // the name keeps at least this much before controls fold
constexpr int foldSteps = 4;
}  // namespace

BlueprintTopBar::BlueprintTopBar(QWidget *parent) : FloatingPanel(parent), m_row(new QHBoxLayout(this)) {
    setObjectName("blueprintTopBar");
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);  // spans the canvas as an overlay
    m_row->setContentsMargins(0, 0, 0, 0);  // contentsMargins already hold the shadow and padding
    m_row->setSpacing(4);
    m_row->setSizeConstraint(QLayout::SetNoConstraint);

    m_back = iconButton("chevron-left", "Back to library");
    m_row->addWidget(m_back);
    m_title = new QWidget;
    m_title->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
    auto *title = new QVBoxLayout(m_title);
    title->setContentsMargins(4, 0, 0, 0);
    title->setSpacing(0);
    m_name = new ElidedLabel("blueprintBarName");
    m_meta = new ElidedLabel("blueprintBarMeta");
    title->addWidget(m_name);
    title->addWidget(m_meta);
    m_row->addWidget(m_title, 1);
    m_draft = pill("Draft");
    m_fixture = pill("Fixture data");
    m_row->addWidget(m_draft, 0, Qt::AlignVCenter);
    m_row->addWidget(m_fixture, 0, Qt::AlignVCenter);
    m_row->addSpacing(8);

    auto *segments = new QFrame;
    segments->setObjectName("blueprintSegments");
    auto *segmentRow = new QHBoxLayout(segments);
    segmentRow->setContentsMargins(3, 3, 3, 3);
    segmentRow->setSpacing(2);
    m_segments = new QButtonGroup(this);
    m_segments->setExclusive(true);
    const auto segment = [&](const QString &text, View view) {
        auto *button = new Button(text);
        button->setObjectName("blueprintSegment");
        button->setFlat(true);
        button->setCheckable(true);
        button->setAccessibleName(text);
        segmentRow->addWidget(button);
        m_segments->addButton(button, int(view));
        return button;
    };
    m_build = segment("Build", View::Build);
    m_tests = segment("Test cases", View::TestCases);
    m_history = segment("Run history", View::RunHistory);
    m_build->setChecked(true);
    m_row->addWidget(segments);
    m_row->addSpacing(8);

    m_validation = new Button;
    m_validation->setObjectName("blueprintValidation");
    m_validation->setFlat(true);
    m_row->addWidget(m_validation);
    m_publish = new Button("Publish");
    m_publish->setToolTip("Publish this draft as a new revision that Conversation and runs can use.");
    m_publish->hide();
    m_row->addWidget(m_publish);
    m_reload = iconButton("refresh-cw", "Discard draft edits");
    m_reload->setToolTip("Discard draft edits · reload the last saved revision");
    m_reload->hide();
    m_row->addWidget(m_reload);
    m_open = iconButton("message-circle", "Open in Conversation");
    m_open->setToolTip("Open in Conversation · run the latest saved revision there");
    m_open->hide();
    m_row->addWidget(m_open);
    m_fullscreen = iconButton("expand", "Fullscreen canvas");
    m_row->addWidget(m_fullscreen);
    m_more = iconButton("ellipsis", "More blueprint actions");
    m_row->addWidget(m_more);
    m_row->addSpacing(4);
    m_run = new Button("Run");
    m_run->setProperty("primary", true);
    m_run->setAccessibleName("Run");
    m_runMenu = new Button;
    m_runMenu->setObjectName("blueprintRunMenu");
    m_runMenu->setProperty("primary", true);
    m_runMenu->setAccessibleName("Run options");
    m_runMenu->setToolTip("Run options");
    m_runMenu->setFixedWidth(28);
    m_row->addWidget(m_run);
    m_row->addWidget(m_runMenu);
    refreshIcons();
    setValidation({});

    connect(m_back, &QAbstractButton::clicked, this, &BlueprintTopBar::backClicked);
    connect(m_segments, &QButtonGroup::idClicked, this, [this](int id) {
        if (View(id) == m_view) return;
        m_view = View(id);
        emit viewChanged(m_view);
    });
    connect(m_validation, &QAbstractButton::clicked, this, [this] { emit issueClicked(m_issues ? 0 : -1); });
    connect(m_publish, &QAbstractButton::clicked, this, &BlueprintTopBar::publishClicked);
    connect(m_reload, &QAbstractButton::clicked, this, &BlueprintTopBar::reloadClicked);
    connect(m_open, &QAbstractButton::clicked, this, &BlueprintTopBar::openInConversationClicked);
    connect(m_fullscreen, &QAbstractButton::clicked, this, &BlueprintTopBar::fullscreenClicked);
    connect(m_more, &QAbstractButton::clicked, this, &BlueprintTopBar::showMoreMenu);
    connect(m_run, &QAbstractButton::clicked, this, &BlueprintTopBar::runClicked);
    connect(m_runMenu, &QAbstractButton::clicked, this, [this] {
        emit runMenuRequested(m_runMenu->mapToGlobal(QPoint(m_runMenu->width(), m_runMenu->height())));
    });
}

QSize BlueprintTopBar::minimumSizeHint() const { return {0, sizeHint().height()}; }

void BlueprintTopBar::setView(View view) {
    m_view = view;
    if (auto *button = m_segments->button(int(view))) button->setChecked(true);
    updateFullscreenButton();
}

void BlueprintTopBar::setBlueprint(const QString &name, const QString &revisionText, bool draft, bool fixture) {
    m_name->setText(name);
    m_meta->setText(revisionText);
    m_meta->setVisible(!revisionText.isEmpty());
    m_draft->setVisible(draft);
    m_fixture->setVisible(fixture);
    fit();
}

void BlueprintTopBar::setValidation(const QStringList &issues) {
    m_issues = int(issues.size());
    m_validationText = issues.isEmpty() ? QString("Ready to run") : countOf(issues.size(), "issue");
    m_validation->setAccessibleName(m_validationText);
    m_validation->setToolTip(issues.isEmpty() ? QString("The workflow has compatible bindings and connected inputs.")
                                              : issues.join('\n'));
    refreshIcons();
    fit();
}

void BlueprintTopBar::setActions(bool canPublish, bool canReload, bool canOpenInConversation, bool canGoBackToConversation) {
    m_publish->setVisible(canPublish);
    m_canReload = canReload;
    m_canOpen = canOpenInConversation;
    m_canGoBack = canGoBackToConversation;
    fit();
}

void BlueprintTopBar::setConnected(bool connected) {
    m_publish->setEnabled(connected);
    m_reload->setEnabled(connected);
}

void BlueprintTopBar::setRunEnabled(bool enabled) {
    m_runEnabled = enabled;
    m_run->setVisible(enabled && m_editable);
    m_runMenu->setVisible(enabled && m_editable);
    fit();
}

void BlueprintTopBar::setEditable(bool editable) {
    m_editable = editable;
    m_build->setEnabled(editable);
    m_history->setEnabled(editable);
    m_validation->setVisible(editable);
    updateFullscreenButton();
    setRunEnabled(m_runEnabled);
}

void BlueprintTopBar::setFullscreenAvailable(bool available) {
    m_fullscreenAvailable = available;
    updateFullscreenButton();
}

void BlueprintTopBar::updateFullscreenButton() {
    const bool shown = m_view == View::Build && m_editable && m_fullscreenAvailable;
    if (m_fullscreen->isHidden() != shown) return;
    m_fullscreen->setVisible(shown);
    fit();
}

void BlueprintTopBar::setFullscreen(bool fullscreen) {
    const QString name = fullscreen ? "Exit fullscreen" : "Fullscreen canvas";
    m_fullscreen->setAccessibleName(name);
    m_fullscreen->setToolTip(name);
    m_fullscreen->setProperty("themeIcon", fullscreen ? "shrink" : "expand");
    refreshIcons();
}

void BlueprintTopBar::showMoreMenu() {
    QMenu menu(this);
    if (m_editable) {
        menu.addAction("Edit details…", this, &BlueprintTopBar::editDetailsClicked);
        menu.addAction("Duplicate", this, &BlueprintTopBar::duplicateClicked);
        menu.addAction("Delete blueprint…", this, &BlueprintTopBar::deleteClicked);
    }
    // Controls the bar folded away for room stay reachable here.
    const bool openFolded = m_canOpen && m_open->isHidden();
    const bool reloadFolded = m_canReload && m_reload->isHidden();
    if (m_canGoBack || openFolded || reloadFolded) menu.addSeparator();
    if (openFolded) menu.addAction("Open in Conversation", this, &BlueprintTopBar::openInConversationClicked);
    if (reloadFolded)
        menu.addAction("Discard draft edits…", this, &BlueprintTopBar::reloadClicked)->setEnabled(m_reload->isEnabled());
    if (m_canGoBack) menu.addAction("Back to Conversation", this, &BlueprintTopBar::backToConversationClicked);
    if (menu.isEmpty()) return;
    menu.exec(m_more->mapToGlobal(QPoint(0, m_more->height())));
}

void BlueprintTopBar::refreshIcons() {
    const QColor secondary(Theme::textSecondary());
    m_fullscreen->setIcon(Theme::coloredIcon(m_fullscreen->property("themeIcon").toString(), secondary, 16));
    m_validation->setIcon(statusDot(QColor(m_issues ? Theme::statusWarning() : Theme::statusSuccess())));
    const QColor ink(Theme::accentText());
    m_run->setIcon(Theme::coloredIcon("play", ink, 14));
    m_runMenu->setIcon(Theme::coloredIcon("chevron-down", ink, 14));
}

void BlueprintTopBar::fit() {
    if (m_fitting) return;
    m_fitting = true;
    const auto fold = [this](int steps) {
        m_validation->setText(steps >= 1 ? QString() : m_validationText);
        m_reload->setVisible(m_canReload && steps < 2);
        m_open->setVisible(m_canOpen && steps < 3);
        m_run->setText(steps >= 4 ? QString() : QString("Run"));
        m_row->invalidate();
    };
    // The title's labels have Ignored widths, so the row's hint leaves them out.
    const auto needed = [this] {
        const int title = qMax(m_name->sizeHint().width(), m_meta->isHidden() ? 0 : m_meta->sizeHint().width());
        return m_row->sizeHint().width() + qMin(title, titleRoom);
    };
    const int room = contentsRect().width();
    fold(0);
    for (int step = 1; step <= foldSteps && needed() > room; ++step) fold(step);
    m_fitting = false;
}

void BlueprintTopBar::resizeEvent(QResizeEvent *event) {
    FloatingPanel::resizeEvent(event);
    fit();
}

void BlueprintTopBar::changeEvent(QEvent *event) {
    FloatingPanel::changeEvent(event);
    if (event->type() == QEvent::StyleChange) refreshIcons();  // a theme change restyles every widget
}
