#pragma once

#include "widgets/button.h"
#include "widgets/toggleswitch.h"

#include <QLabel>
#include <QScrollArea>
#include <QScrollBar>
#include <QStringList>
#include <QWidget>

namespace TextFit {
// Text that is elided ("Organi…"), cut at an edge or squeezed too short on the visible widgets under root.
// True when part of w sticks out sideways past an ancestor that clips it (so a button is cut at the edge).
inline bool cutSideways(QWidget *w, int *overhang = nullptr) {
    const QRect own(w->mapTo(w->window(), QPoint(0, 0)), w->size());
    for (QWidget *ancestor = w->parentWidget(); ancestor && ancestor != w->window(); ancestor = ancestor->parentWidget()) {
        const QRect visible(ancestor->mapTo(ancestor->window(), QPoint(0, 0)), ancestor->size());
        if (own.right() < visible.left() || own.left() > visible.right()) return false;   // wholly outside: scrolled away, not cut
        const int over = qMax(visible.left() - own.left(), own.right() - visible.right());
        if (over > 1) { if (overhang) *overhang = over; return true; }
    }
    return false;
}

inline QStringList clippedText(QWidget *root) {
    QStringList found;
    // Scroll areas that cannot scroll sideways must show their content at full width. A vertical bar that takes
    // layout width (not the floating kind) used to cost 3px and now costs 12px, so also name areas that only fit
    // with the old width.
    QList<QScrollArea *> scrollAreas = root->findChildren<QScrollArea *>();
    if (auto *self = qobject_cast<QScrollArea *>(root)) scrollAreas.prepend(self);
    for (auto *area : scrollAreas) {
        if (!area->isVisible() || !area->widget() || !area->widgetResizable()) continue;
        if (area->horizontalScrollBarPolicy() != Qt::ScrollBarAlwaysOff) continue;
        const int needs = area->widget()->minimumSizeHint().width();
        const int shown = area->viewport()->width();
        const bool barTakesWidth = area->verticalScrollBar()->isVisible();
        if (needs > shown + 1)
            found << QString("scroll area %1: content needs %2px, %3px visible%4").arg(area->objectName().isEmpty() ? "(unnamed)" : area->objectName()).arg(needs).arg(shown)
                         .arg(barTakesWidth && needs <= shown + 9 ? " (fits without the scrollbar's extra width)" : "");
    }
    for (auto *w : root->findChildren<QWidget *>()) {
        if (!w->isVisible() || w->width()<4 || w->height()<4) continue;
        if (qobject_cast<Button *>(w) || qobject_cast<ToggleSwitch *>(w)) {
            int over = 0;
            if (cutSideways(w, &over)) found << QString("%1 \"%2\" cut %3px by its container").arg(qobject_cast<Button *>(w) ? "button" : "switch", qobject_cast<QAbstractButton *>(w)->text()).arg(over);
        }
        if (auto *button=qobject_cast<Button *>(w)) {
            if (!button->textFits()) found<<QString("button \"%1\" %2px").arg(button->text()).arg(button->width());
        } else if (auto *toggle=qobject_cast<ToggleSwitch *>(w)) {
            if (!toggle->text().isEmpty() && toggle->width()<toggle->labelLeft()+toggle->fontMetrics().horizontalAdvance(toggle->text()))
                found<<QString("switch \"%1\"").arg(toggle->text());
        } else if (auto *label=qobject_cast<QLabel *>(w)) {
            if (label->text().isEmpty() || label->textFormat()==Qt::RichText || label->pixmap().isNull()==false) continue;
            const int need=label->wordWrap()?label->heightForWidth(label->width()):label->sizeHint().height();
            if (label->height()+1<need) found<<QString("label \"%1\"#%4 %2<%3px high (font %5px, parent %6px)").arg(label->text().left(30).replace('\n',' ')).arg(label->height()).arg(need).arg(label->objectName()).arg(label->font().pixelSize()).arg(label->parentWidget()?label->parentWidget()->height():0);
            // Single-line labels that may not shrink must show all their text (Ignored-width labels clip on purpose).
            if (!label->wordWrap() && label->sizePolicy().horizontalPolicy()!=QSizePolicy::Ignored && !label->text().contains('\n')
                && label->width()+1<label->fontMetrics().horizontalAdvance(label->text()))
                found<<QString("label \"%1\"#%4 %2<%3px wide").arg(label->text().left(30)).arg(label->width()).arg(label->fontMetrics().horizontalAdvance(label->text())).arg(label->objectName());
        }
    }
    found.removeDuplicates();
    return found;
}

// "[Compact] page: nothing elided or cut" style message with the offenders appended.
inline QString describe(const QString &size, const QString &page, const QStringList &found) {
    return QString("text fit [%1] %2: nothing elided or cut").arg(size, page) + (found.isEmpty() ? QString() : " — " + found.join("; "));
}
inline const char *sizeName(int size) { return size == 0 ? "Compact" : size == 2 ? "Large" : "Default"; }
}
