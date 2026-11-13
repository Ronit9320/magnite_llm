#include "widgets/namedialog.h"

#include "panels/greeting.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include "widgets/button.h"
#include <QVBoxLayout>

NameDialog::NameDialog(const QString &suggestion, QWidget *parent)
    : SettingsDetailDialog(parent) {
    setWindowTitle("Welcome to Magnite Space");
    setModal(true);
    setMinimumWidth(380);
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 22, 24, 20);
    layout->setSpacing(10);
    auto *title = new QLabel("What should I call you?", this);
    title->setObjectName("hubDialogTitle");
    title->setProperty("windowDragRegion", true);
    auto *heading = new QHBoxLayout;
    heading->addWidget(title, 1);
    heading->addWidget(Ui::dialogCloseButton(this));
    layout->addLayout(heading);
    auto *hint = new QLabel("Used only for the greeting on a new chat. It stays on this computer, and you can change it any time in Settings.", this);
    hint->setObjectName("cardMeta");
    hint->setWordWrap(true);
    layout->addWidget(hint);
    m_input = new QLineEdit(Greeting::cleanName(suggestion), this);
    m_input->setObjectName("nameInput");
    m_input->setAccessibleName("Your name");
    m_input->setPlaceholderText("Your name");
    m_input->setMaxLength(80);
    layout->addWidget(m_input);
    auto *buttons = new QHBoxLayout();
    buttons->addStretch();
    auto *skip = new Button("Skip", this);
    skip->setObjectName("secondaryBtn");
    auto *save = new Button("Save", this);
    save->setObjectName("primaryBtn");
    save->setDefault(true);
    buttons->addWidget(skip);
    buttons->addWidget(save);
    layout->addLayout(buttons);
    connect(skip, &QPushButton::clicked, this, &QDialog::reject);
    connect(save, &QPushButton::clicked, this, &QDialog::accept);
    connect(m_input, &QLineEdit::returnPressed, this, &QDialog::accept);
    m_input->selectAll();
    m_input->setFocus();
}

QString NameDialog::name() const { return Greeting::cleanName(m_input->text()); }
