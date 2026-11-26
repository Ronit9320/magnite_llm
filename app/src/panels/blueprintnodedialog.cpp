#include "panels/blueprintnodedialog.h"

#include "panels/blueprintnodefields.h"
#include "panels/blueprintpreview.h"
#include "widgets/settingswindow.h"

#include <QDialogButtonBox>
#include <QFrame>
#include <QLabel>
#include <QScreen>
#include <QScrollArea>
#include <QVBoxLayout>

namespace {
class NodeDialog : public SettingsDetailDialog {
public:
    NodeDialog(BlueprintPreview &preview, QWidget *parent, const QString &kind, const QJsonObject &existing)
        : SettingsDetailDialog(parent) {
        setObjectName("blueprintNodeDialog");
        setWindowTitle(existing.isEmpty() ? "Create node" : "Configure node");
        auto *body = new QVBoxLayout(this);
        body->setSpacing(7);
        // Same title treatment as the other editor dialogs.
        auto *title = new QLabel(windowTitle());
        title->setTextFormat(Qt::PlainText);
        title->setObjectName("headingLabel");
        body->addWidget(title);
        auto *divider = new QFrame;
        divider->setObjectName("blueprintRule");
        divider->setFixedHeight(1);
        body->addWidget(divider);

        auto *scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        scroll->setFrameShape(QFrame::NoFrame);
        m_fields = new BlueprintNodeFields(preview, kind, existing);
        scroll->setWidget(m_fields);
        body->addWidget(scroll, 1);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Cancel);
        buttons->addButton(existing.isEmpty() ? "Create node" : "Save changes", QDialogButtonBox::AcceptRole);
        body->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        connect(buttons, &QDialogButtonBox::accepted, this, [this] {
            if (m_fields->showError()) return;
            m_result = m_fields->node();
            accept();
        });
        m_fields->firstField()->setFocus();
        setMinimumWidth(500);
        // Tall enough that ports and settings show without scrolling in most configurations.
        const int available = parent && parent->screen() ? parent->screen()->availableGeometry().height() : 900;
        resize(540, qMin(660, int(available * .9)));
    }

    QJsonObject result() const { return m_result; }

private:
    BlueprintNodeFields *m_fields;
    QJsonObject m_result;
};
}  // namespace

std::optional<QJsonObject> configureBlueprintNode(BlueprintPreview &preview, QWidget *parent,
    const QString &kind, const QJsonObject &existing) {
    NodeDialog dialog(preview, parent, kind, existing);
    if (dialog.exec() != QDialog::Accepted) return std::nullopt;
    return dialog.result();
}
