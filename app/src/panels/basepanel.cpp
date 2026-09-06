#include "panels/basepanel.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

BasePanel::BasePanel(const QString &title, QWidget *parent)
    : QWidget(parent)
    , m_title(title)
    , m_contentLayout(new QVBoxLayout())
    , m_subtitleLabel(nullptr)
{
    auto *root = new QVBoxLayout(this);

    auto *titleLabel = new QLabel(title, this);
    titleLabel->setStyleSheet("font-size: 22px; font-weight: bold;");

    m_subtitleLabel = new QLabel(this);
    m_subtitleLabel->setStyleSheet("color: #666;");
    m_subtitleLabel->setWordWrap(true);
    m_subtitleLabel->setVisible(false);

    root->addWidget(titleLabel);
    root->addWidget(m_subtitleLabel);
    root->addSpacing(8);

    auto *content = new QWidget(this);
    content->setLayout(m_contentLayout);
    root->addWidget(content, 1);
}

void BasePanel::setHeaderSubtitle(const QString &subtitle) {
    m_subtitleLabel->setText(subtitle);
    m_subtitleLabel->setVisible(!subtitle.isEmpty());
}
