#pragma once

#include <QSet>
#include <QString>
#include <QStringList>

// Startup repair notices ("models.db was set aside...") come from the backend process. A relaunched backend starts
// a new, shorter list, so "already shown" is remembered by text, never by position in the list.
class NoticeTracker {
public:
    // The notices in `current` not shown before; they count as shown from now on.
    QStringList unseen(const QStringList &current) {
        QStringList fresh;
        for (const QString &notice : current) {
            if (m_shown.contains(notice)) continue;
            m_shown.insert(notice);
            fresh << notice;
        }
        return fresh;
    }

private:
    QSet<QString> m_shown;
};
