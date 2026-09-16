#include "panels/ragdoccard.h"

#include "theme.h"

#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QStyle>
#include <QVBoxLayout>

namespace {

// Per-extension tint for the shared file-text glyph so pdf/txt/md/html read
// as distinct document types without shipping four SVG assets.
QColor typeColor(const QString &ext) {
    if (ext == QStringLiteral("pdf")) return QColor(248, 113, 113);   // soft red
    if (ext == QStringLiteral("md")) return QColor(52, 211, 153);     // mint
    if (ext == QStringLiteral("html")) return QColor(251, 191, 36);   // amber
    return QColor(Theme::textSecondary());                            // txt + others
}

}  // namespace

RagDocCard::RagDocCard(const QString &filePath, QWidget *parent)
    : QFrame(parent)
    , m_path(filePath)
    , m_selected(false)
{
    setObjectName(QStringLiteral("ragDocCard"));
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    const QFileInfo fi(filePath);
    const QString ext = fi.suffix().toLower();

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(14, 10, 14, 10);
    row->setSpacing(12);

    const QIcon icon = Theme::coloredIcon(QStringLiteral("file-text"), typeColor(ext), 18);
    if (!icon.isNull()) {
        auto *iconLabel = new QLabel(this);
        iconLabel->setPixmap(icon.pixmap(18, 18));
        row->addWidget(iconLabel, 0, Qt::AlignVCenter);
    }

    auto *text = new QVBoxLayout();
    text->setSpacing(2);
    auto *name = new QLabel(fi.fileName().isEmpty() ? filePath : fi.fileName(), this);
    name->setObjectName(QStringLiteral("bodyLabel"));
    text->addWidget(name);
    auto *path = new QLabel(filePath, this);
    path->setObjectName(QStringLiteral("cardMeta"));
    text->addWidget(path);
    row->addLayout(text, 1);

    auto *tag = new QLabel(ext.isEmpty() ? QStringLiteral("FILE") : ext.toUpper(), this);
    tag->setObjectName(QStringLiteral("adapterPill"));
    row->addWidget(tag, 0, Qt::AlignVCenter);
}

void RagDocCard::setSelected(bool selected) {
    if (m_selected == selected) return;
    m_selected = selected;
    setProperty("selected", selected);
    style()->unpolish(this);
    style()->polish(this);
}

void RagDocCard::mousePressEvent(QMouseEvent *event) {
    emit cardClicked(this, (event->modifiers() & Qt::ControlModifier) != 0);
    QFrame::mousePressEvent(event);
}