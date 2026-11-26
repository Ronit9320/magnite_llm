#pragma once

#include "widgets/floatingpanel.h"

#include <QJsonObject>

class BlueprintPreview;
class QButtonGroup;
class QHBoxLayout;
class QLabel;
class QStackedWidget;
class QVBoxLayout;

// A small movable window that explains a node: what it does, its pins and its last run.
class BlueprintDetails : public FloatingPanel {
    Q_OBJECT
public:
    explicit BlueprintDetails(const BlueprintPreview &preview, QWidget *parent = nullptr);

    // node is empty for a library entry; anchor is the rectangle (in the parent) it opens beside.
    void showFor(const QString &operation, const QJsonObject &node, const QJsonObject &graph,
                 const QJsonObject &lastStep, const QRect &anchor);
    // The explanations for every operation, keyed by operation id (from :/blueprints/node-docs.json).
    static QJsonObject docs();
    // The node the window describes, or empty for a library entry.
    QString nodeId() const { return m_nodeId; }

signals:
    void editRequested(const QString &nodeId);
    void testRequested(const QString &nodeId);
    void addRequested();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void place(const QRect &anchor);

    const BlueprintPreview &m_preview;
    QWidget *m_header;
    QLabel *m_glyph;
    QLabel *m_title;
    QLabel *m_subtitle;
    QButtonGroup *m_tabs;
    QStackedWidget *m_pages;
    QVBoxLayout *m_overview;
    QVBoxLayout *m_ports;
    QVBoxLayout *m_lastRun;
    QWidget *m_lastRunTab;
    QHBoxLayout *m_footer;
    QString m_nodeId;
    QPoint m_dragFrom;
};
