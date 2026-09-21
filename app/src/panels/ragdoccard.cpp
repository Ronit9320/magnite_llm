#include "panels/ragdoccard.h"

#include "theme.h"

#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QStyle>
#include <QVBoxLayout>

namespace {

// Per-kind tint for the shared file-text glyph so pdf/txt/md/html read
// as distinct document types without shipping four SVG assets.
QColor typeColor(const QString &kind) {
    Q_UNUSED(kind);
    return QColor(Theme::accentIce());
}

}  // namespace

RagDocCard::RagDocCard(const QJsonObject &data, QWidget *parent)
    : QFrame(parent)
    , m_id(data.value("id").toString())
    , m_path(data.value("path").toString())
    , m_selected(false)
{
    setObjectName(QStringLiteral("ragDocCard"));
    setCursor(Qt::PointingHandCursor);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);

    const QString name = data.value("name").toString();
    const QString kind = data.value("kind").toString().toLower();
    const int chunks = data.value("chunks").toInt();

    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(14, 10, 14, 10);
    row->setSpacing(12);

    const QIcon icon = Theme::coloredIcon(QStringLiteral("file-text"), typeColor(kind), 18);
    if (!icon.isNull()) {
        auto *iconLabel = new QLabel(this);
        iconLabel->setPixmap(icon.pixmap(18, 18));
        row->addWidget(iconLabel, 0, Qt::AlignVCenter);
    }

    auto *text = new QVBoxLayout();
    text->setSpacing(2);
    auto *nameLabel = new QLabel(name.isEmpty() ? m_id : name, this);
    nameLabel->setObjectName(QStringLiteral("bodyLabel"));
    text->addWidget(nameLabel);
    const QString meta = m_path.isEmpty()
        ? QStringLiteral("uploaded · %1 chunk(s)").arg(chunks)
        : QStringLiteral("%1 · %2 chunk(s)").arg(m_path).arg(chunks);
    auto *pathLabel = new QLabel(meta, this);
    pathLabel->setObjectName(QStringLiteral("cardMeta"));
    text->addWidget(pathLabel);
    row->addLayout(text, 1);

    auto *tag = new QLabel(kind.isEmpty() ? QStringLiteral("FILE")
                                          : kind.toUpper(), this);
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