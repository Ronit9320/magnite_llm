#pragma once

#include <QLocale>
#include <QString>

// "1 adapter", "2 adapters", "1,204 chunks": counts in UI text never read "adapter(s)".
inline QString countOf(qint64 n, const QString &singular, const QString &plural = QString()) {
    const QString noun = n == 1 ? singular : (plural.isEmpty() ? singular + QLatin1Char('s') : plural);
    return QLocale().toString(n) + QLatin1Char(' ') + noun;
}

#include <QDateTime>

// One way to show a backend timestamp: "19 Nov 2026" or "19 Nov 2026, 12:24" in
// local time. Text that is not a timestamp is returned unchanged.
inline QString displayDate(const QString &iso, bool withTime = false) {
    QDateTime time = QDateTime::fromString(iso, Qt::ISODateWithMs);
    if (!time.isValid()) time = QDateTime::fromString(iso, Qt::ISODate);
    if (!time.isValid()) return iso;
    time = time.toLocalTime();
    return time.toString(withTime ? QStringLiteral("d MMM yyyy, HH:mm") : QStringLiteral("d MMM yyyy"));
}
