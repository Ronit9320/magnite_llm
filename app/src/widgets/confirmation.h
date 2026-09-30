#pragma once
#include <QMessageBox>
#include <QPushButton>

namespace Ui {
inline bool confirmRemoval(QWidget *parent, const QString &title, const QString &message,
                           const QString &action = QStringLiteral("Delete")) {
    QMessageBox dialog(QMessageBox::Question,title,message,QMessageBox::Cancel,parent);
    dialog.setTextFormat(Qt::PlainText);
    auto *remove = dialog.addButton(action,QMessageBox::DestructiveRole);
    dialog.setDefaultButton(QMessageBox::Cancel);
    dialog.setEscapeButton(QMessageBox::Cancel);
    dialog.exec();
    return dialog.clickedButton() == remove;
}
}
