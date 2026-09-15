#include "panels/basepanel.h"

#include "theme.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QStyle>
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

    auto *titleLabel = new QLabel(title, this);
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
    root->addWidget(content, 1);
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