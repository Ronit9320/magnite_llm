#pragma once
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <QStringList>
#include <QUrl>

struct DocsSection {
    QString id, title, html, text;
};
struct DocsArticle {
    QString id, title, shortTitle, group, type, icon, description;
    QStringList sources, related;
    QList<DocsSection> sections;
};
struct DocsSource {
    QString id, label, location;
    bool local = false;
};
struct DocsExample {
    QString id, name, filename, content;
};
struct DocsMatch {
    QString articleId, sectionId, heading, snippet;
    int score = 0;
};

// Read-only, embedded editorial content. No network or backend dependency.
class DocsLibrary {
  public:
    bool load(const QString &resource = QStringLiteral(":/docs/catalog.json"));
    QString error() const { return m_error; }
    const QList<DocsArticle> &articles() const { return m_articles; }
    const DocsArticle *article(const QString &id) const;
    const DocsSource *source(const QString &id) const;
    const DocsExample *example(const QString &id) const;
    QStringList groups() const;
    QStringList types() const;
    QList<DocsMatch> search(const QString &query) const;
    QStringList sourceIds() const { return m_sources.keys(); }
    static QString plainText(const QString &html);
    static QStringList terms(const QString &query);
    static QString highlight(const QString &text, const QStringList &words);
    static QUrl articleLink(const QString &id, const QString &section = {});
    static bool parseArticleLink(const QUrl &link, QString *id, QString *section);

  private:
    QList<DocsArticle> m_articles;
    QMap<QString, DocsSource> m_sources;
    QMap<QString, DocsExample> m_examples;
    QString m_error;
};
