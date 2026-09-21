#include "panels/basepanel.h"

#include "theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QStyle>
#include <QScrollArea>
#include <QVBoxLayout>

BasePanel::BasePanel(const QString &title, QWidget *parent)
    : QWidget(parent)
    , m_title(title)
    , m_contentLayout(new QVBoxLayout())
    , m_subtitleLabel(nullptr)
{
    auto *root = new QVBoxLayout(this);
    // Design doc §4: panels pad 32px around their content.
    root->setContentsMargins(Theme::panelPadding(), Theme::panelPadding(),
                             Theme::panelPadding(), Theme::panelPadding());
    root->setSpacing(Theme::spacingBase());

    const QString displayTitle = title == "Models" ? "Model library"
        : title == "Chat" ? "Playground" : title;
    auto *titleLabel = new QLabel(displayTitle, this);
    titleLabel->setObjectName("panelTitle");

    m_subtitleLabel = new QLabel(this);
    m_subtitleLabel->setObjectName("panelSubtitle");
    m_subtitleLabel->setWordWrap(true);
    m_subtitleLabel->setVisible(false);

    root->addWidget(titleLabel);
    root->addWidget(m_subtitleLabel);
    root->addSpacing(Theme::spacingBase());

    auto *content = new QWidget(this);
    content->setLayout(m_contentLayout);
    m_contentLayout->setContentsMargins(0,0,0,0);
    m_contentLayout->setSpacing(24);
    auto *scroll = new QScrollArea(this);
    scroll->setObjectName("cardScroll");
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    scroll->viewport()->setAutoFillBackground(false);
    scroll->setWidget(content);
    root->addWidget(scroll,1);
}

void BasePanel::setHeaderSubtitle(const QString &subtitle) {
    m_subtitleLabel->setText(subtitle);
    m_subtitleLabel->setVisible(!subtitle.isEmpty());
}

void BasePanel::applyStatusTone(QLabel *label, const QString &tone) {
    label->setProperty("state", tone);
    label->style()->unpolish(label);
    label->style()->polish(label);
}
