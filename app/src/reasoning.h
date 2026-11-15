#pragma once

#include <QString>

// Splits a model reply into the part the model "thought" and the answer itself.
// Reasoning models (Qwen3, DeepSeek-R1, …) wrap their thinking in <think>…</think>. Some chat
// templates already open the block in the prompt, so the reply then carries only the closing tag.
// Pure text handling so it can run on every streamed update and be checked without widgets.
namespace Reasoning {

struct Parsed {
    QString thinking;          // trimmed; empty when there is none
    QString answer;            // what follows the thinking (leading blank lines dropped)
    bool hasThinking = false;  // a non-empty thinking part exists
    bool open = false;         // the block has not been closed yet (still thinking, or cut off)
};

namespace detail {
inline const QString &openTag() { static const QString tag = QStringLiteral("<think>"); return tag; }
inline const QString &closeTag() { static const QString tag = QStringLiteral("</think>"); return tag; }

// Length of the longest suffix of `text` that is a proper prefix of `tag` (a tag cut mid-stream).
inline int partialTagSuffix(const QString &text, const QString &tag) {
    for (int length = qMin<int>(tag.size() - 1, text.size()); length > 0; --length)
        if (text.endsWith(tag.left(length))) return length;
    return 0;
}

inline QString dropLeadingBlankLines(QString text) {
    int i = 0;
    while (i < text.size() && text.at(i).isSpace()) ++i;
    if (i == text.size()) return QString();
    // Keep the first line's own indentation: only whole blank lines before it go.
    const int lineStart = text.lastIndexOf(QLatin1Char('\n'), i - 1) + 1;
    return text.mid(i > 0 ? lineStart : 0);
}
} // namespace detail

inline Parsed parse(const QString &text) {
    using namespace detail;
    Parsed out;
    const QString lead = text.trimmed();

    // A reply that is still only the start of "<think>" shows nothing yet.
    if (!lead.isEmpty() && lead.size() < openTag().size() && openTag().startsWith(lead)) {
        out.open = true;
        return out;
    }

    QString thinking;
    if (lead.startsWith(openTag())) {
        const int start = text.indexOf(openTag()) + openTag().size();
        const int end = text.indexOf(closeTag(), start);
        if (end < 0) {
            thinking = text.mid(start);
            thinking.chop(partialTagSuffix(thinking, closeTag()));
            out.open = true;
        } else {
            thinking = text.mid(start, end - start);
            out.answer = dropLeadingBlankLines(text.mid(end + closeTag().size()));
        }
    } else if (const int end = text.indexOf(closeTag()); end >= 0 && !text.contains(openTag())) {
        // The template opened the block in the prompt: everything before the closing tag is thinking.
        thinking = text.left(end);
        out.answer = dropLeadingBlankLines(text.mid(end + closeTag().size()));
    } else {
        out.answer = text;
    }

    out.thinking = thinking.trimmed();
    out.hasThinking = !out.thinking.isEmpty();
    return out;
}

// The text to send back to the model as history: the answer without its thinking. A reply that was
// cut off while still thinking has no answer, so it is kept as it is.
inline QString answerOnly(const QString &text) {
    const Parsed parsed = parse(text);
    if (!parsed.hasThinking && !text.contains(detail::closeTag())) return text;
    return parsed.answer.trimmed().isEmpty() ? text : parsed.answer;
}

} // namespace Reasoning
