#pragma once
#include "panels/basepanel.h"
#include "panels/docslibrary.h"
#include <QMap>
#include <QPointer>
#include <QSet>
#include <functional>

class QBoxLayout;
class QComboBox;
class QFrame;
class QLineEdit;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QScrollArea;
class QTimer;
class QVBoxLayout;
class DocsCard;

class DocsPanel : public BasePanel {
    Q_OBJECT
  public:
    explicit DocsPanel(QWidget *parent = nullptr);
    ~DocsPanel() override;
    const DocsLibrary &library() const { return m_library; }
    QString currentArticleId() const;
    QString activeSection() const { return m_activeSection; }
    bool openArticle(const QString &id, const QString &section = {});
    bool openLink(const QUrl &link);
    void showOverview();
    void showLibrary(bool saved = false);
    void showSearch(const QString &query);
    void setFocusMode(bool focus);
    bool focusMode() const { return m_focus; }
    void flushPosition();
    std::function<bool(const QUrl &)> openExternalUrl;
    std::function<QString(const QString &)> chooseExampleDestination;
  signals:
    void focusModeChanged(bool focus);

  protected:
    void resizeEvent(QResizeEvent *event) override;
    void showEvent(QShowEvent *event) override;
    void hideEvent(QHideEvent *event) override;
    void changeEvent(QEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;

  private:
    struct Route {
        QString kind = "overview", id, section, query, topic, type;
        bool operator==(const Route &) const = default;
    };
    void setupUi();
    void navigate(const Route &route, bool replace = false);
    void goHistory(int direction);
    void render(bool keepOffset = false, const QString &focusObject = {});
    void renderNavigation();
    void renderOverview(QVBoxLayout *body);
    void renderCollection(QVBoxLayout *body);
    void renderSearch(QVBoxLayout *body);
    void renderReader(QVBoxLayout *body);
    void addArticleBody(QVBoxLayout *body, const QString &html);
    void addExample(QVBoxLayout *body);
    void updateExample();
    void downloadExample();
    void toggleSaved(const QString &id);
    void updateContents();
    void scrollToSection(const QString &section, bool focus = true);
    void setStatus(const QString &message);
    void showSources();
    void openSource(const QString &id);
    void handleTextLink(const QUrl &url);
    void toggleNavigation();
    void closeNavigation();
    void updateResponsive();
    void scheduleRefresh();
    int mainWidth() const;
    QString layoutClass() const;
    DocsCard *articleCard(const DocsArticle &article, bool libraryCard = false);
    QPushButton *action(const QString &text, const std::function<void()> &callback,
                        const char *objectName = "secondaryBtn");
    void savePosition();

    DocsLibrary m_library;
    Route m_route;
    QList<Route> m_history;
    int m_historyIndex = 0;
    QSet<QString> m_saved, m_collapsed;
    QString m_lastArticle, m_activeSection, m_example = "jsonl", m_layoutClass;
    bool m_large = false, m_focus = false, m_compact = false, m_navOpen = false, m_wrap = true;
    bool m_restoring = false, m_ready = false, m_refreshPending = false;
    int m_generation = 0;
    QWidget *m_shell = nullptr, *m_navigation = nullptr, *m_content = nullptr, *m_toc = nullptr;
    QFrame *m_readerTools = nullptr;
    QWidget *m_scrim = nullptr;
    QBoxLayout *m_shellLayout = nullptr;
    QVBoxLayout *m_navigationBody = nullptr;
    QScrollArea *m_topicScroll = nullptr, *m_scroll = nullptr;
    QLineEdit *m_search = nullptr;
    QLabel *m_status = nullptr, *m_toolbarSubtitle = nullptr;
    QPushButton *m_menu = nullptr, *m_back = nullptr, *m_forward = nullptr, *m_savedButton = nullptr;
    QTimer *m_saveTimer = nullptr, *m_resizeTimer = nullptr;
    QMap<QString, QPointer<QWidget>> m_sections;
    QMap<QString, QPointer<QPushButton>> m_sectionLinks;
    QMap<QString, QPointer<QPushButton>> m_groupButtons;
    QComboBox *m_topicFilter = nullptr, *m_typeFilter = nullptr;
    QPointer<QPlainTextEdit> m_exampleText;
    QPointer<QLabel> m_exampleFile;
    QPointer<QPushButton> m_copyExample, m_wrapExample;
    QMap<QString, QPointer<QPushButton>> m_exampleTabs;
};
