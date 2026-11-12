#pragma once

#include "theme.h"
#include "widgets/button.h"
#include "widgets/settingswindow.h"

#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QVBoxLayout>

namespace Ui {
class LibraryDialog : public SettingsDetailDialog {
public:
    LibraryDialog(QWidget *parent, const QString &title, const QString &action)
        : SettingsDetailDialog(parent), m_layout(new QVBoxLayout(this)),
          m_accept(new Button(action, this)) {
        setWindowTitle(title);
        setMinimumWidth(465);
        m_layout->setContentsMargins(24, 22, 24, 20);
        m_layout->setSpacing(12);
        auto *header = new QWidget(this);
        header->setProperty("windowDragRegion", true);
        auto *heading = new QHBoxLayout(header);
        heading->setContentsMargins(0, 0, 0, 0);
        auto *label = new QLabel(title, header);
        label->setObjectName("hubDialogTitle");
        heading->addWidget(label, 1);
        auto *close = new Button(header);
        close->setObjectName("hubDialogClose");
        close->setFlat(true);
        close->setFixedSize(30, 30);
        close->setIconSize(QSize(16, 16));
        close->setIcon(Theme::coloredIcon("window-close", QColor(Theme::textSecondary()), 16));
        close->setAccessibleName("Close dialog");
        close->setAutoDefault(false);
        connect(close, &QPushButton::clicked, this, &QDialog::reject);
        heading->addWidget(close);
        m_layout->addWidget(header);
        m_accept->setProperty("primary", true);
        m_accept->setAutoDefault(false);
        connect(m_accept, &QPushButton::clicked, this, &QDialog::accept);
    }
    QVBoxLayout *form() const { return m_layout; }
    Button *acceptButton() const { return m_accept; }
    void addActions() {
        auto *row = new QHBoxLayout;
        auto *cancel = new Button("Cancel", this);
        cancel->setAutoDefault(false);
        connect(cancel, &QPushButton::clicked, this, &QDialog::reject);
        row->addStretch();
        row->addWidget(cancel);
        row->addWidget(m_accept);
        m_layout->addLayout(row);
    }
private:
    QVBoxLayout *m_layout;
    Button *m_accept;
};

inline QString chooseLibraryImport(QWidget *parent, const QString &title,
                                   const QString &description, const QString &filter,
                                   const QString &suffix) {
    LibraryDialog dialog(parent, title, "Import");
    auto *caption = new QLabel(description, &dialog);
    caption->setObjectName("hubBody");
    caption->setWordWrap(true);
    dialog.form()->addWidget(caption);
    auto *row = new QHBoxLayout;
    auto *path = new QLineEdit(&dialog);
    path->setPlaceholderText("Choose a " + suffix + " file");
    path->setAccessibleName("File path");
    row->addWidget(path, 1);
    auto *browse = new Button("Browse…", &dialog);
    browse->setAutoDefault(false);
    row->addWidget(browse);
    dialog.form()->addLayout(row);
    QObject::connect(browse, &QPushButton::clicked, &dialog, [&] {
        const auto selected = QFileDialog::getOpenFileName(&dialog, title, path->text(), filter);
        if (!selected.isEmpty()) path->setText(selected);
    });
    dialog.acceptButton()->setEnabled(false);
    QObject::connect(path, &QLineEdit::textChanged, &dialog, [&](const QString &value) {
        const QFileInfo file(value.trimmed());
        dialog.acceptButton()->setEnabled(file.isFile() && file.suffix().compare(suffix, Qt::CaseInsensitive) == 0);
    });
    dialog.addActions();
    return dialog.exec() == QDialog::Accepted ? QFileInfo(path->text().trimmed()).absoluteFilePath() : QString();
}
}
