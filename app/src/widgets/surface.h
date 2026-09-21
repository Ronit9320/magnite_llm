#pragma once
#include <QFrame>
#include <QLabel>
#include <QVBoxLayout>
#include "theme.h"

namespace Surface {
inline QVBoxLayout *card(QWidget *parent, const QString &title = {}) {
    auto *frame = new QFrame(parent);
    frame->setObjectName("surfaceCard");
    auto *layout = new QVBoxLayout(frame);
    layout->setContentsMargins(24,24,24,24);
    layout->setSpacing(16);
    if (!title.isEmpty()) {
        auto *label = new QLabel(title,frame);
        label->setObjectName("headingLabel");
        layout->addWidget(label);
    }
    return layout;
}
inline void field(QVBoxLayout *layout, const QString &title, QWidget *editor) {
    auto *label = new QLabel(title,layout->parentWidget());
    label->setObjectName("cardMeta");
    layout->addWidget(label);
    layout->addWidget(editor);
}
}
