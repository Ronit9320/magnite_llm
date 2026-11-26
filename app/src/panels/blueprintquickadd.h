#pragma once

#include "panels/blueprintlibrarydock.h"
#include "widgets/floatingpanel.h"

#include <QColor>
#include <functional>

class BlueprintPreview;
class QLabel;
class QLineEdit;
class QListWidget;
class QWidget;

// The quick-add menu: a search field and the node library as a keyboard-driven list of rows
// (glyph, name, description), opened at the cursor over the canvas.
class BlueprintQuickAdd : public FloatingPanel {
    Q_OBJECT
public:
    using Filter = std::function<bool(const BlueprintLibraryDock::Entry &)>;
    // The input of an entry's node that would receive a dropped wire, or empty when none fits.
    using Accepts = std::function<QString(const BlueprintLibraryDock::Entry &)>;

    // Item data roles the rows carry (the display role is the entry's title).
    enum Role { EntryRole = Qt::UserRole, DetailRole, ColorRole, IconRole, AcceptsRole, GroupRole };

    explicit BlueprintQuickAdd(BlueprintPreview &preview, QWidget *parent = nullptr);

    // Shows the menu at a position in the parent widget, listing entries that pass filter.
    void open(const QPoint &position, Filter filter = {});
    // The part of the parent the menu must stay inside (the whole parent when empty).
    void setBounds(const QRect &bounds) { m_bounds = bounds; }
    // Shows the menu for a wire dropped on empty canvas: only nodes that accept the wire,
    // each with the input it connects to, under a line naming the wire's type.
    void openForWire(const QPoint &position, const QString &typeLabel, const QColor &typeColor, Accepts accepts);
    void dismiss();

signals:
    void accepted(const BlueprintLibraryDock::Entry &entry);
    void detailsRequested(const BlueprintLibraryDock::Entry &entry);
    void closed();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void place(const QPoint &position);
    void refresh();
    void moveSelection(int step);
    void accept(bool details);

    BlueprintPreview &m_preview;
    QLineEdit *m_search = nullptr;
    QWidget *m_context = nullptr;
    QLabel *m_contextDot = nullptr;
    QLabel *m_contextText = nullptr;
    QListWidget *m_list = nullptr;
    Filter m_filter;
    Accepts m_accepts;
    QRect m_bounds;
};
