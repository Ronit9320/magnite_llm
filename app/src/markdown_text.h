#pragma once

#include <QRegularExpression>
#include <QString>
#include <QStringList>

// Model replies are Markdown (**bold**, lists, `code`). The bubble renders them with Qt's Markdown
// support; prepare() adjusts the text first so it reads the way a chat reply should.
namespace MarkdownText {

namespace detail {
inline bool isFence(const QString &line) {
    const QString t = line.trimmed();
    return t.startsWith(QLatin1String("```")) || t.startsWith(QLatin1String("~~~"));
}

inline bool isListItem(const QString &line) {
    static const QRegularExpression item(QStringLiteral("^\\s*([-*+]|\\d+[.)])\\s"));
    return item.match(line).hasMatch();
}

// Raw HTML in a reply is shown as typed, not rendered: "\<" is a literal "<" in Markdown. Inline
// code is left alone (nothing in it is interpreted).
inline QString escapeHtml(const QString &line) {
    if (!line.contains(QLatin1Char('<'))) return line;
    QString out;
    bool inCode = false;
    for (const QChar c : line) {
        if (c == QLatin1Char('`')) inCode = !inCode;
        if (c == QLatin1Char('<') && !inCode) out += QLatin1Char('\\');
        out += c;
    }
    return out;
}
} // namespace detail

// - single newlines become line breaks (Markdown would join them into one line);
// - a blank line between paragraphs keeps one line of space (Qt draws no paragraph gap);
// - raw HTML tags are shown literally.
// Code fences are left exactly as they are.
inline QString prepare(const QString &text) {
    QStringList lines = text.split(QLatin1Char('\n'));
    QStringList out;
    bool fenced = false;
    for (int i = 0; i < lines.size(); ++i) {
        const QString &line = lines.at(i);
        if (detail::isFence(line)) {
            fenced = !fenced;
            out << line;
            continue;
        }
        if (fenced) {
            out << line;
            continue;
        }
        if (line.trimmed().isEmpty()) {
            // A run of blank lines after a text line, with more text after it.
            int next = i;
            while (next < lines.size() && lines.at(next).trimmed().isEmpty()) ++next;
            if (i > 0 && !lines.at(i - 1).trimmed().isEmpty() && next < lines.size()) {
                const bool looseList = detail::isListItem(lines.at(i - 1)) && detail::isListItem(lines.at(next));
                out << QString();
                if (!looseList) out << QStringLiteral("&nbsp;") << QString();
                i = next - 1;
            } else {
                out << line;
            }
            continue;
        }
        QString prepared = detail::escapeHtml(line);
        if (i + 1 < lines.size() && !lines.at(i + 1).trimmed().isEmpty() && !detail::isFence(lines.at(i + 1)) &&
            !prepared.endsWith(QLatin1String("  ")))
            prepared += QLatin1String("  ");
        out << prepared;
    }
    return out.join(QLatin1Char('\n'));
}

} // namespace MarkdownText
