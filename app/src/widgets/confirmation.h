#pragma once

#include "widgets/librarydialog.h"

namespace Ui {
class MessageDialog : public LibraryDialog {
public:
    MessageDialog(QWidget *parent, const QString &title, const QString &message,
                  const QString &action, bool destructive = false, bool withCancel = true)
        : LibraryDialog(parent, title, action) {
        setObjectName("appMessageDialog");
        auto *body = new QLabel(message, this);
        body->setObjectName("appMessageBody");
        body->setTextFormat(Qt::PlainText);
        body->setWordWrap(true);
        body->setTextInteractionFlags(Qt::TextSelectableByMouse);
        body->setMaximumWidth(540);
        form()->addWidget(body);
        if (destructive) {
            acceptButton()->setProperty("primary", false);
            acceptButton()->setObjectName("destructiveBtn");
        }
        addActions(withCancel, true);
    }
};

inline bool confirmRemoval(QWidget *parent, const QString &title, const QString &message,
                           const QString &action = QStringLiteral("Delete")) {
    MessageDialog dialog(parent, title, message, action, true);
    return dialog.exec() == QDialog::Accepted;
}

inline bool confirmAction(QWidget *parent, const QString &title, const QString &message,
                          const QString &action) {
    MessageDialog dialog(parent, title, message, action);
    return dialog.exec() == QDialog::Accepted;
}

inline void showNotice(QWidget *parent, const QString &title, const QString &message) {
    MessageDialog dialog(parent, title, message, QObject::tr("OK"), false, false);
    dialog.exec();
}
}
