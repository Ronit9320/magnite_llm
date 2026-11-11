#include "panels/docslibrary.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QTextDocument>
#include <QUrlQuery>
#include <algorithm>

namespace {
QStringList strings(const QJsonValue &value) {
    QStringList result;
    for (const auto &item : value.toArray())
        if (item.isString())
            result << item.toString();
    return result;
}
bool validId(const QString &id) { return QRegularExpression("^[a-z0-9]+(?:-[a-z0-9]+)*$").match(id).hasMatch(); }
} // namespace
bool DocsLibrary::load(const QString &resource) {
    m_articles.clear();
    m_sources.clear();
    m_examples.clear();
    m_error.clear();
    auto fail = [this](const QString &message) {
        m_articles.clear();
        m_sources.clear();
        m_examples.clear();
        m_error = message;
        return false;
    };
    QFile file(resource);
    if (!file.open(QIODevice::ReadOnly))
        return fail("The local reference library could not be opened.");
    QJsonParseError parse;
    const auto doc = QJsonDocument::fromJson(file.readAll(), &parse);
    if (parse.error != QJsonParseError::NoError || !doc.isObject() || doc.object()["format_version"].toInt() != 1)
        return fail("The local reference library has an unsupported format.");
    const auto data = doc.object();
    const auto sources = data["sources"].toObject();
    for (auto it = sources.begin(); it != sources.end(); ++it) {
        const auto o = it.value().toObject();
        const QString location = o["url"].toString();
        const bool local = location.startsWith("reference/");
        const QUrl url(location);
        if (!validId(it.key()) || o["label"].toString().isEmpty() ||
            (!local && (url.scheme() != "https" || url.host().isEmpty())))
            return fail("A reference source is invalid.");
        if (local && (!QRegularExpression("^reference/[A-Z_]+\\.md$").match(location).hasMatch() ||
                      !QFile::exists(":/docs/" + location)))
            return fail("A bundled reference is missing.");
        m_sources.insert(it.key(), DocsSource{it.key(), o["label"].toString(), location, local});
    }
    const auto related = data["related"].toObject();
    QSet<QString> ids;
    for (const auto &value : data["articles"].toArray()) {
        const auto o = value.toObject();
        DocsArticle article;
        article.id = o["id"].toString();
        article.title = o["title"].toString();
        article.shortTitle = o["short"].toString();
        article.group = o["group"].toString();
        article.type = o["type"].toString();
        article.icon = o["icon"].toString();
        article.description = o["description"].toString();
        article.sources = strings(o["sources"]);
        article.related = strings(related[article.id]);
        if (!validId(article.id) || ids.contains(article.id) || article.title.isEmpty() || article.group.isEmpty())
            return fail("An article identity is invalid.");
        ids.insert(article.id);
        QSet<QString> sections;
        for (const auto &v : o["sections"].toArray()) {
            const auto s = v.toArray();
            if (s.size() != 3)
                return fail("An article section is invalid.");
            DocsSection section{s[0].toString(), s[1].toString(), s[2].toString(), {}};
            if (!validId(section.id) || sections.contains(section.id) || section.title.isEmpty())
                return fail("An article section identity is invalid.");
            sections.insert(section.id);
            section.text = plainText(section.html);
            article.sections << section;
        }
        if (article.sections.isEmpty())
            return fail("An article has no content.");
        for (const auto &source : article.sources)
            if (!m_sources.contains(source))
                return fail("An article source is missing.");
        m_articles << article;
    }
    if (m_articles.isEmpty())
        return fail("The reference library is empty.");
    for (const auto &article : m_articles)
        for (const auto &id : article.related)
            if (!ids.contains(id) || id == article.id)
                return fail("A related article link is invalid.");
    const auto examples = data["examples"].toObject();
    for (auto it = examples.begin(); it != examples.end(); ++it) {
        const auto o = it.value().toObject();
        const auto filename = o["filename"].toString();
        if (!validId(it.key()) || !QRegularExpression("^[a-z0-9-]+\\.(jsonl|csv)$").match(filename).hasMatch() ||
            o["content"].toString().isEmpty())
            return fail("A dataset example is invalid.");
        m_examples.insert(it.key(), DocsExample{it.key(), o["name"].toString(), filename, o["content"].toString()});
    }
    return true;
}
const DocsArticle *DocsLibrary::article(const QString &id) const {
    for (const auto &article : m_articles)
        if (article.id == id)
            return &article;
    return nullptr;
}
const DocsSource *DocsLibrary::source(const QString &id) const {
    auto it = m_sources.constFind(id);
    return it == m_sources.cend() ? nullptr : &it.value();
}
const DocsExample *DocsLibrary::example(const QString &id) const {
    auto it = m_examples.constFind(id);
    return it == m_examples.cend() ? nullptr : &it.value();
}
QStringList DocsLibrary::groups() const {
    QStringList result;
    for (const auto &a : m_articles)
        if (!result.contains(a.group))
            result << a.group;
    return result;
}
QStringList DocsLibrary::types() const {
    QStringList result;
    for (const auto &a : m_articles)
        if (!result.contains(a.type))
            result << a.type;
    return result;
}
QString DocsLibrary::plainText(const QString &html) {
    QTextDocument doc;
    doc.setHtml(html);
    return doc.toPlainText().simplified();
}
QStringList DocsLibrary::terms(const QString &query) {
    auto words = query.toLower().simplified().split(' ', Qt::SkipEmptyParts);
    for (auto &word : words) {
        if (word == "oom" || word == "vram")
            word = "memory";
        else if (word == "finetuning" || word == "fine-tuning")
            word = "training";
        else if (word == "over-fitting")
            word = "overfitting";
    }
    return words;
}
QList<DocsMatch> DocsLibrary::search(const QString &query) const {
    const auto words = terms(query);
    QList<DocsMatch> result;
    if (words.isEmpty())
        return result;
    for (const auto &article : m_articles) {
        QString all = article.title + ' ' + article.shortTitle + ' ' + article.description + ' ' + article.group;
        for (const auto &s : article.sections)
            all += ' ' + s.title + ' ' + s.text;
        bool matches = true;
        for (const auto &word : words)
            if (!all.contains(word, Qt::CaseInsensitive)) {
                matches = false;
                break;
            }
        if (!matches)
            continue;
        int score = 0;
        for (const auto &word : words) {
            if ((article.title + ' ' + article.shortTitle).contains(word, Qt::CaseInsensitive))
                score += 12;
            if (article.description.contains(word, Qt::CaseInsensitive))
                score += 5;
        }
        const DocsSection *best = &article.sections.first();
        int bestScore = -1;
        for (const auto &s : article.sections) {
            int rank = 0;
            for (const auto &word : words) {
                if (s.title.contains(word, Qt::CaseInsensitive))
                    rank += 5;
                if (s.text.contains(word, Qt::CaseInsensitive))
                    ++rank;
            }
            if (rank > bestScore) {
                best = &s;
                bestScore = rank;
            }
        }
        const QString content = bestScore > 0 ? best->text : article.description;
        int matchAt = 0;
        for (const auto &word : words) {
            const int pos = content.indexOf(word, 0, Qt::CaseInsensitive);
            if (pos >= 0) {
                matchAt = pos;
                break;
            }
        }
        int start = qMax(0, matchAt - 55);
        if (start > 0) {
            const int space = content.lastIndexOf(' ', start);
            if (space >= 0)
                start = space + 1;
        }
        int end = qMin(int(content.size()), start + 210);
        if (end < content.size()) {
            const int space = content.lastIndexOf(' ', end);
            if (space > start + 100)
                end = space;
        }
        const QString snippet = (start ? QStringLiteral("…") : QString()) + content.mid(start, end - start) +
                                (end < content.size() ? QStringLiteral("…") : QString());
        result << DocsMatch{article.id, bestScore > 0 ? best->id : QString(), best->title, snippet, score + bestScore};
    }
    std::stable_sort(result.begin(), result.end(),
                     [](const DocsMatch &a, const DocsMatch &b) { return a.score > b.score; });
    return result;
}
QString DocsLibrary::highlight(const QString &text, const QStringList &words) {
    QList<QPair<int, int>> ranges;
    for (const auto &word : words) {
        if (word.isEmpty())
            continue;
        int offset = 0;
        while (offset < text.size()) {
            const int at = text.indexOf(word, offset, Qt::CaseInsensitive);
            if (at < 0)
                break;
            ranges << qMakePair(at, at + int(word.size()));
            offset = at + word.size();
        }
    }
    std::sort(ranges.begin(), ranges.end());
    QList<QPair<int, int>> merged;
    for (const auto &r : ranges) {
        if (!merged.isEmpty() && r.first <= merged.last().second)
            merged.last().second = qMax(merged.last().second, r.second);
        else
            merged << r;
    }
    QString result;
    int end = 0;
    for (const auto &r : merged) {
        result += text.mid(end, r.first - end).toHtmlEscaped() + "<b>" +
                  text.mid(r.first, r.second - r.first).toHtmlEscaped() + "</b>";
        end = r.second;
    }
    return result + text.mid(end).toHtmlEscaped();
}
QUrl DocsLibrary::articleLink(const QString &id, const QString &section) {
    QUrl url;
    url.setScheme("magnite-docs");
    url.setHost("article");
    url.setPath('/' + id);
    if (!section.isEmpty()) {
        QUrlQuery query;
        query.addQueryItem("section", section);
        url.setQuery(query);
    }
    return url;
}
bool DocsLibrary::parseArticleLink(const QUrl &link, QString *id, QString *section) {
    if (link.scheme() != "magnite-docs" || link.host() != "article" || !link.userInfo().isEmpty() || link.port() != -1)
        return false;
    const auto article = link.path().mid(1);
    const auto anchor = QUrlQuery(link).queryItemValue("section");
    if (!validId(article) || (!anchor.isEmpty() && !validId(anchor)))
        return false;
    *id = article;
    *section = anchor;
    return true;
}
