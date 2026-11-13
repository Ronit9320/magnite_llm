#pragma once

#include <QObject>
#include <QPointer>
#include <QScrollBar>
#include <QVector>

class QAbstractScrollArea;
class QEvent;

class QWidget;

// Shows a scroll area's vertical bar at the far right edge of its window instead of inside the
// area's own margins. The area keeps scrolling normally (wheel, keys); its built-in bar is turned
// off and this overlay bar stays in sync, so it can be dragged and styled like any QScrollBar.
class EdgeScrollBar : public QObject {
    Q_OBJECT
public:
    static constexpr int kWidth = 12;   // keep in step with the QScrollBar track width in console.qss

    // Attaches once per area; calling it again returns the existing helper.
    static EdgeScrollBar *attach(QAbstractScrollArea *area);
    static EdgeScrollBar *of(const QAbstractScrollArea *area);
    QScrollBar *bar() const { return m_bar; }

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    explicit EdgeScrollBar(QAbstractScrollArea *area);
    ~EdgeScrollBar() override;
    void rewire();
    void syncRange();
    void place();

    QAbstractScrollArea *m_area;
    QPointer<QScrollBar> m_bar;
    QVector<QPointer<QObject>> m_watched;
};
