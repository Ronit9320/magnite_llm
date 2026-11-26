#pragma once

#include "panels/blueprintlibrarydock.h"
#include "widgets/floatingpanel.h"

#include <functional>

class BlueprintPreview;
class QLineEdit;
class QListWidget;

// The quick-add menu: a search field and the node library as a keyboard-driven list,
// opened at the cursor over the canvas.
class BlueprintQuickAdd : public FloatingPanel {
    Q_OBJECT
public:
    using Filter = std::function<bool(const BlueprintLibraryDock::Entry &)>;

    explicit BlueprintQuickAdd(BlueprintPreview &preview, QWidget *parent = nullptr);

    // Shows the menu at a position in the parent widget, listing entries that pass filter.
    void open(const QPoint &position, Filter filter = {});
    void dismiss();

signals:
    void accepted(const BlueprintLibraryDock::Entry &entry);
    void detailsRequested(const BlueprintLibraryDock::Entry &entry);
    void closed();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void refresh();
    void moveSelection(int step);
    void accept(bool details);

    BlueprintPreview &m_preview;
    QLineEdit *m_search;
    QListWidget *m_list;
    Filter m_filter;
};
