#include "testing/docs_tests.h"
#include "panels/docspanel.h"
#include "theme.h"
#include "widgets/docscontentview.h"
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>
#include <cstdio>
namespace {
void pump(int ms = 90) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < ms) {
        QApplication::processEvents();
        QThread::msleep(2);
    }
}
void key(QWidget *target, int code, Qt::KeyboardModifiers modifiers = Qt::NoModifier) {
    QKeyEvent e(QEvent::KeyPress, code, modifiers);
    QApplication::sendEvent(target, &e);
}
QPushButton *button(QWidget *parent, const QString &name) { return parent->findChild<QPushButton *>(name); }
} // namespace
int runDocsChecks(const QString &captures) {
    int failures = 0;
    auto check = [&](bool ok, const char *message) {
        if (!ok)
            ++failures;
        fprintf(stderr, "[docs-check] %s %s\n", ok ? "ok:" : "FAIL:", message);
    };
    QSettings settings;
    QMap<QString, QVariant> prior;
    for (const auto &name : settings.allKeys())
        prior[name] = settings.value(name);
    const bool dark = Theme::isDark();
    settings.remove("docs");
    QTemporaryDir output;
    {
        DocsPanel docs;
        docs.resize(1240, 900);
        docs.show();
        pump(220);
        check(docs.library().error().isEmpty() && docs.library().articles().size() == 16 &&
                  docs.library().groups().size() == 5,
              "sixteen complete offline articles in five subjects");
        for (const auto &article : docs.library().articles()) {
            check(docs.openArticle(article.id), qPrintable("open article " + article.id));
            pump();
            check(docs.findChildren<DocsTextView *>().size() >= article.sections.size(),
                  "all article sections render as native text");
            for (const auto &section : article.sections) {
                check(docs.openLink(DocsLibrary::articleLink(article.id, section.id)),
                      "section links open known sections");
            }
        }
        check(!docs.openArticle("unknown") && !docs.openArticle("memory", "unknown") &&
                  !docs.openLink(QUrl("file:///etc/passwd")) &&
                  !docs.openLink(QUrl("magnite-docs://article/../memory")),
              "invalid articles, sections and protocols are rejected");
        check(!docs.library().search("JSONL").isEmpty() &&
                  docs.library().search("JSONL").first().articleId == "dataset-format",
              "search ranks dataset formats for JSONL");
        check(!docs.library().search("OOM").isEmpty() && docs.library().search("zzzzzz absent").isEmpty(),
              "memory aliases and missing search terms behave correctly");
        check(!DocsLibrary::highlight("<script>& test", {"test"}).contains("<script>"),
              "search snippets escape markup");
        docs.openArticle("dataset-format");
        pump();
        auto *code = docs.findChild<QPlainTextEdit *>("docsExampleCode");
        auto *copy = button(&docs, "docsCopyExample");
        check(code && copy, "dataset examples offer native text and actions");
        if (code && copy) {
            for (const auto &format : QStringList{"jsonl", "messages", "csv"}) {
                QPushButton *tab = nullptr;
                for (auto *b : docs.findChildren<QPushButton *>("docsExampleTab"))
                    if (b->property("format").toString() == format)
                        tab = b;
                if (tab)
                    tab->click();
                check(tab && code->toPlainText() == docs.library().example(format)->content,
                      "format tab displays the exact approved sample");
                copy->click();
                check(QApplication::clipboard()->text() == code->toPlainText(), "copy preserves UTF-8 dataset content");
            }
            QString savedPath;
            docs.chooseExampleDestination = [&](const QString &filename) {
                savedPath = output.path() + "/" + filename;
                return savedPath;
            };
            button(&docs, "docsDownloadExample")->click();
            QFile saved(savedPath);
            check(saved.open(QIODevice::ReadOnly) && QString::fromUtf8(saved.readAll()) == code->toPlainText(),
                  "download writes the selected example atomically");
            button(&docs, "docsWrapExample")->click();
            check(code->lineWrapMode() == QPlainTextEdit::NoWrap, "long lines can scroll without clipping");
        }
        docs.openArticle("model-learning", "objective");
        pump();
        check(docs.activeSection() == "objective", "deep section navigation highlights the active section");
        auto *save = button(&docs, "docsSaveArticle");
        save->click();
        check(settings.value("docs/savedArticles").toStringList().contains("model-learning"),
              "saved article persists locally");
        auto *scroll = docs.findChild<QScrollArea *>("docsScroll");
        scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum() / 2);
        pump(240);
        const auto position = settings.value("docs/positions/model-learning").toMap();
        check(position.value("offset").toInt() > 0, "reading position is saved quietly");
        docs.showOverview();
        pump();
        docs.openArticle("model-learning");
        pump();
        check(qAbs(scroll->verticalScrollBar()->value() - position.value("offset").toInt()) < 8,
              "returning to an article restores its place");
        docs.setFocusMode(true);
        pump();
        check(docs.focusMode() && !docs.findChild<QWidget *>("docsNavigation")->isVisible(),
              "focus mode hides subject navigation");
        key(button(&docs, "docsFocus"), Qt::Key_Escape);
        pump();
        check(!docs.focusMode(), "Escape restores navigation from focus mode");
        button(&docs, "docsTextSize")->click();
        pump();
        check(settings.value("docs/textSize") == "large", "larger reading text persists");
        docs.showOverview();
        pump();
        auto *featureTitle = docs.findChild<QLabel *>("docsFeatureTitle");
        check(featureTitle && featureTitle->height() >= featureTitle->heightForWidth(featureTitle->width()),
              "overview feature title is fully visible");
        docs.showLibrary();
        pump();
        auto *subject = docs.findChild<QComboBox *>("docsTopicFilter");
        const int dataSubject = subject->findData("Understanding data");
        subject->setCurrentIndex(dataSubject);
        QMetaObject::invokeMethod(subject, "activated", Q_ARG(int, dataSubject));
        pump();
        check(docs.findChild<QScrollArea *>("docsScroll")->widget()->findChildren<DocsCard *>().size() == 3,
              "subject filter narrows the library to its three data references");
        docs.openArticle("data-quality");
        pump();
        button(&docs, "docsHistoryBack")->click();
        pump();
        check(docs.findChild<QComboBox *>("docsTopicFilter")->currentData().toString() == "Understanding data",
              "history restores the subject filter");
        docs.showLibrary(true);
        pump();
        check(docs.findChildren<DocsCard *>().size() >= 1, "saved shelf contains saved references");
        docs.showSearch("validation");
        pump();
        check(!docs.currentArticleId().size() && docs.findChildren<DocsCard *>().size() > 0,
              "search renders local matching references");
        int externalCalls = 0;
        docs.openExternalUrl = [&](const QUrl &url) {
            ++externalCalls;
            return url.scheme() == "https";
        };
        docs.openArticle("lora");
        pump();
        for (auto *b : docs.findChildren<QPushButton *>("docsSourceLink")) {
            auto *source = docs.library().source(b->property("sourceId").toString());
            if (source && !source->local) {
                b->click();
                break;
            }
        }
        check(externalCalls == 1, "external references open only on explicit activation");
        docs.openArticle("first-run");
        pump();
        bool localOpened = false;
        for (auto *b : docs.findChildren<QPushButton *>("docsSourceLink")) {
            const auto *source = docs.library().source(b->property("sourceId").toString());
            if (source && source->local) {
                QTimer::singleShot(40, qApp, [&] {
                    if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                        auto *reference = dialog->findChild<DocsTextView *>();
                        localOpened = reference && !reference->toPlainText().isEmpty() &&
                                      dialog->windowFlags().testFlag(Qt::FramelessWindowHint);
                        dialog->accept();
                    }
                });
                b->click();
                break;
            }
        }
        check(localOpened && externalCalls == 1, "bundled Markdown opens offline in an opaque app-owned dialog");
        docs.resize(640, 760);
        docs.showOverview();
        pump(180);
        check(button(&docs, "docsMenu")->isVisible(), "compact windows expose the topic drawer");
        button(&docs, "docsMenu")->click();
        pump();
        check(docs.findChild<QWidget *>("docsNavigation")->isVisible() && !scroll->isEnabled(),
              "topic drawer blocks the underlying content");
        key(button(&docs, "docsCloseNavigation"), Qt::Key_Escape);
        pump();
        check(!docs.findChild<QWidget *>("docsNavigation")->isVisible() && scroll->isEnabled(),
              "Escape closes the topic drawer");
        if (!captures.isEmpty()) {
            QDir().mkpath(captures);
            for (bool shade : {false, true}) {
                Theme::setDark(shade);
                pump(180);
                const QString suffix = shade ? "dark" : "light";
                docs.resize(1240, 900);
                docs.showOverview();
                pump(220);
                docs.grab().save(captures + "/docs-home-" + suffix + ".png");
                for (const auto &id :
                     QStringList{"model-learning", "data-distribution", "dataset-format", "generalization"}) {
                    docs.openArticle(id, docs.library().article(id)->sections.first().id);
                    pump(180);
                    scroll->verticalScrollBar()->setValue(0);
                    docs.grab().save(captures + "/docs-" + id + "-" + suffix + ".png");
                }
                docs.resize(640, 760);
                docs.showOverview();
                pump(180);
                docs.grab().save(captures + "/docs-compact-" + suffix + ".png");
            }
        }
        docs.flushPosition();
        docs.hide();
    }
    {
        check(settings.value("docs/savedArticles").toStringList().contains("model-learning"),
              "saved preferences remain after theme and capture changes");
        DocsPanel reopened;
        check(reopened.library().error().isEmpty(), "reference library can be reopened without a backend");
        reopened.openArticle("model-learning");
        reopened.show();
        pump();
        auto *restored = button(&reopened, "docsSaveArticle");
        check(restored && restored->isChecked(), "saved article survives panel recreation");
    }
    settings.clear();
    for (auto it = prior.begin(); it != prior.end(); ++it)
        settings.setValue(it.key(), it.value());
    Theme::setDark(dark);
    settings.sync();
    fprintf(stderr, "[docs-check] %d failure(s)\n", failures);
    return failures;
}
