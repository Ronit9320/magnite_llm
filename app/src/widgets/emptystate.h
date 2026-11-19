#pragma once

#include "theme.h"

#include <QColor>
#include <QFrame>
#include <QLabel>
#include <QVBoxLayout>

// The one empty-state block used by lists: icon, title, one line of help.
// A caller may add an action under it with addAction(). Styled by
// QFrame#emptyState / QLabel#emptyStateTitle / QLabel#emptyStateBody.
class EmptyState : public QFrame {
public:
    EmptyState(const QString &icon, const QString &title, const QString &body, QWidget *parent = nullptr)
        : QFrame(parent) {
        setObjectName(QStringLiteral("emptyState"));
        setAttribute(Qt::WA_StyledBackground, true);
        auto *layout = new QVBoxLayout(this);
        layout->setContentsMargins(24, 28, 24, 28);
        layout->setSpacing(8);
        layout->addStretch(1);
        m_icon = new QLabel(this);
        m_icon->setObjectName(QStringLiteral("emptyStateIcon"));
        m_icon->setAlignment(Qt::AlignCenter);
        layout->addWidget(m_icon);
        m_title = new QLabel(this);
        m_title->setObjectName(QStringLiteral("emptyStateTitle"));
        m_title->setAlignment(Qt::AlignCenter);
        m_title->setWordWrap(true);
        m_title->setTextFormat(Qt::PlainText);
        layout->addWidget(m_title);
        m_body = new QLabel(this);
        m_body->setObjectName(QStringLiteral("emptyStateBody"));
        m_body->setAlignment(Qt::AlignCenter);
        m_body->setWordWrap(true);
        m_body->setTextFormat(Qt::PlainText);
        layout->addWidget(m_body);
        m_actions = new QVBoxLayout;
        m_actions->setContentsMargins(0, 6, 0, 0);
        m_actions->setAlignment(Qt::AlignHCenter);
        layout->addLayout(m_actions);
        layout->addStretch(1);
        setIcon(icon);
        setContent(title, body);
    }

    void setIcon(const QString &icon) {
        m_icon->setProperty("themeIcon", icon);
        m_icon->setProperty("themeIconSize", 28);
        m_icon->setProperty("themeIconTone", "muted");
        m_icon->setPixmap(Theme::iconPixmap(icon, QColor(Theme::textMuted()), 28, devicePixelRatioF()));
        m_icon->setVisible(!icon.isEmpty());
    }

    void setContent(const QString &title, const QString &body) {
        m_title->setText(title);
        m_title->setVisible(!title.isEmpty());
        m_body->setText(body);
        m_body->setVisible(!body.isEmpty());
        setAccessibleName(title);
        setAccessibleDescription(body);
    }

    QString title() const { return m_title->text(); }
    QString body() const { return m_body->text(); }
    void addAction(QWidget *action) { m_actions->addWidget(action, 0, Qt::AlignHCenter); }

private:
    QLabel *m_icon = nullptr;
    QLabel *m_title = nullptr;
    QLabel *m_body = nullptr;
    QVBoxLayout *m_actions = nullptr;
};
