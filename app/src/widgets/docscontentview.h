#pragma once
#include "widgets/button.h"
#include <QTextBrowser>

// A button with a real layout: cards retain native focus/activation and shared
// motion.
class DocsCard : public Button {
    Q_OBJECT
  public:
    explicit DocsCard(QWidget *parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;
    int heightForWidth(int width) const override;
};

// Rich text, not a web view. External/embedded resources are never fetched.
class DocsTextView : public QTextBrowser {
    Q_OBJECT
  public:
    explicit DocsTextView(QWidget *parent = nullptr, bool expand = true);
    void setContent(const QString &html);
    void setReferenceMarkdown(const QString &markdown);
    QSize minimumSizeHint() const override { return QSize(0, 0); }

  protected:
    void resizeEvent(QResizeEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    QVariant loadResource(int type, const QUrl &name) override;

  private:
    void reflow();
    void applyTextTheme();
    bool m_expand, m_reflowing = false;
};

class DocsDiagram : public QWidget {
  public:
    enum Kind { Learning, Tokens, LoRA, Data, Distribution, Steps };
    explicit DocsDiagram(Kind kind, QWidget *parent = nullptr);
    // A flow of 2–5 labelled steps joined by arrows (articles' concept-flow blocks).
    explicit DocsDiagram(const QStringList &steps, QWidget *parent = nullptr);
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return QSize(0, 80); }
    int heightForWidth(int width) const override;

  protected:
    void paintEvent(QPaintEvent *event) override;

  private:
    bool vertical(int width) const;
    Kind m_kind;
    QStringList m_steps;
};
