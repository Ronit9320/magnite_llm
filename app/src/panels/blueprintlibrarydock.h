#pragma once

#include <QHash>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QWidget>

class BlueprintPreview;
class QButtonGroup;
class QLineEdit;
class QListWidget;
class QStackedWidget;
class QVBoxLayout;

// The node library docked at the left of the canvas: operations by family, saved nodes,
// and a list of the nodes already in this workflow.
class BlueprintLibraryDock : public QWidget {
    Q_OBJECT
public:
    // One thing that can be added: a catalog operation, or a node saved to the library.
    struct Entry {
        QString operation;
        QJsonObject savedNode;
        QString savedId;
        QString savedName;
        QByteArray toJson() const;
        static Entry fromJson(const QByteArray &json);
    };
    static constexpr const char *savedGroup = "Saved";

    explicit BlueprintLibraryDock(BlueprintPreview &preview, QWidget *parent = nullptr);

    // Library entries grouped by family (BlueprintStyle::families() order, then Saved).
    // Legacy and order-example operations are left out; empty groups are omitted.
    static QList<QPair<QString, QList<Entry>>> groups(const BlueprintPreview &preview, const QString &query,
                                                      const QString &family);
    // The row title an entry shows: the operation's label, or the saved node's name.
    static QString title(const BlueprintPreview &preview, const Entry &entry);

    void setGraph(const QJsonObject &graph, const QHash<QString, QString> &warnings);
    void setCurrentNode(const QString &id);
    void focusSearch();

signals:
    void addRequested(const BlueprintLibraryDock::Entry &entry);
    void detailsRequested(const BlueprintLibraryDock::Entry &entry);
    void nodeActivated(const QString &id);

private:
    void refreshLibrary();
    QWidget *row(const Entry &entry);

    BlueprintPreview &m_preview;
    QButtonGroup *m_tabs;
    QStackedWidget *m_pages;
    QLineEdit *m_search;
    QButtonGroup *m_chips;
    QString m_family;
    QVBoxLayout *m_rows;
    QListWidget *m_workflow;
};
