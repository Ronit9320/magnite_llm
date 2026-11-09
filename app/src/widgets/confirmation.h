#pragma once
#include <QMessageBox>
#include <QPushButton>
#include <QLayout>
#include "widgets/button.h"

namespace Ui {
inline bool confirmRemoval(QWidget *parent, const QString &title, const QString &message,
                           const QString &action = QStringLiteral("Delete")) {
    QMessageBox dialog(QMessageBox::NoIcon,title,message,QMessageBox::NoButton,parent);
    dialog.layout()->setContentsMargins(24,20,24,20);
    dialog.setTextFormat(Qt::PlainText);
    auto *cancel=new Button("Cancel",&dialog);
    auto *remove=new Button(action,&dialog); remove->setObjectName("destructiveBtn");
    dialog.addButton(cancel,QMessageBox::RejectRole); dialog.addButton(remove,QMessageBox::DestructiveRole);
    dialog.setDefaultButton(cancel); dialog.setEscapeButton(cancel);
    dialog.exec();
    return dialog.clickedButton() == remove;
}
}
