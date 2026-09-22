#include "panels/modelcard.h"

#include "theme.h"

#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QProgressBar>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace {

QString formatNumber(double value, const QString &suffix = QString()) {
    if (value <= 0) return QStringLiteral("-");
    return QStringLiteral("%1%2").arg(value, 0, 'f', value < 10 ? 1 : 0).arg(suffix);
}

}  // namespace

ModelCard::ModelCard(const QJsonObject &data, bool isHub, QWidget *parent)
    : QFrame(parent)
    , m_id(isHub ? data.value("repo_id").toString() : data.value("id").toString())
    , m_progressRow(nullptr)
    , m_progressLabel(nullptr)
    , m_progressBar(nullptr)
{
    setObjectName(QStringLiteral("modelCard"));
    setMinimumWidth(230);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setCursor(Qt::PointingHandCursor);

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    // Row 1: name (Space Grotesk) + architecture pill.
    auto *nameRow = new QHBoxLayout();
    nameRow->setSpacing(8);
    auto *name = new QLabel(m_id, this);
    name->setObjectName(QStringLiteral("cardTitle"));
    name->setWordWrap(true);
    nameRow->addWidget(name, 1);
    if (!isHub) {
        const QString arch = data.value("architecture").toString();
        if (!arch.isEmpty()) {
            auto *badge = new QLabel(arch, this);
            badge->setObjectName(QStringLiteral("archBadge"));
            nameRow->addWidget(badge, 0, Qt::AlignTop);
        }
    }
    layout->addLayout(nameRow);

    // Row 2: source icon + name, then size for local models.
    const QString source = data.value("source").toString();
    const QString iconName = source.compare("ollama", Qt::CaseInsensitive) == 0
        ? QStringLiteral("database") : QStringLiteral("download");
    auto *metaRow = new QHBoxLayout();
    metaRow->setSpacing(6);
    const QIcon srcIcon = Theme::coloredIcon(iconName, QColor(Theme::textSecondary()), 14);
    if (!srcIcon.isNull()) {
        auto *iconLabel = new QLabel(this);
        iconLabel->setObjectName(QStringLiteral("cardMeta"));
        iconLabel->setProperty("themeIcon", iconName);
        iconLabel->setPixmap(srcIcon.pixmap(14, 14));
        metaRow->addWidget(iconLabel);
    }
    auto *sourceLabel = new QLabel(
        source.isEmpty() ? (isHub ? QStringLiteral("HuggingFace") : QStringLiteral("Local"))
                         : source,
        this);
    sourceLabel->setObjectName(QStringLiteral("cardMeta"));
    metaRow->addWidget(sourceLabel);

    if (!isHub) {
        auto *dot = new QLabel(QStringLiteral("·"), this);
        dot->setObjectName(QStringLiteral("cardMeta"));
        metaRow->addWidget(dot);
        auto *size = new QLabel(formatNumber(data.value("size_mb").toDouble(), " MB"), this);
        size->setObjectName(QStringLiteral("cardMeta"));
        metaRow->addWidget(size);
    }
    metaRow->addStretch(1);
    layout->addLayout(metaRow);

    // Row 3: license on both views; downloads/likes on Hub hits.
    auto *footRow = new QHBoxLayout();
    footRow->setSpacing(6);
    const QString license = data.value("license").toString();
    auto *licenseLabel = new QLabel(license.isEmpty() ? QStringLiteral("Open") : license, this);
    licenseLabel->setObjectName(QStringLiteral("cardMeta"));
    footRow->addWidget(licenseLabel);
    if (isHub) {
        auto *stats = new QLabel(
            QStringLiteral("%1 downloads · %2 likes")
                .arg(data.value("downloads").toInt())
                .arg(data.value("likes").toInt()),
            this);
        stats->setObjectName(QStringLiteral("cardMeta"));
        footRow->addWidget(stats);
    }
    footRow->addStretch(1);
    layout->addLayout(footRow);

    appendProgressRow();
    auto *actions = new QHBoxLayout();
    auto *state = new QLabel(isHub ? "Available on the Hub" : "✓ Ready locally",this);
    state->setObjectName("cardMeta");
    actions->addWidget(state,1);
    auto *action = new QPushButton(isHub ? "Download" : "Load model",this);
    action->setObjectName("primaryBtn");
    actions->addWidget(action);
    layout->addLayout(actions);
    connect(action,&QPushButton::clicked,this,[this]() { emit actionRequested(this); });
}

void ModelCard::appendProgressRow() {
    auto *progressRow = new QHBoxLayout();
    progressRow->setSpacing(8);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setObjectName(QStringLiteral("cardProgress"));
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(false);
    m_progressBar->setFixedHeight(4);
    progressRow->addWidget(m_progressBar, 1);

    m_progressLabel = new QLabel("0%", this);
    m_progressLabel->setObjectName(QStringLiteral("monoLabel"));
    progressRow->addWidget(m_progressLabel);

    m_progressRow = new QWidget(this);
    m_progressRow->setLayout(progressRow);
    (static_cast<QVBoxLayout *>(layout()))->addWidget(m_progressRow);
    m_progressRow->setVisible(false);
}

void ModelCard::setProgress(double percent) {
    if (!m_progressRow) return;
    if (percent < 0) {
        m_progressRow->setVisible(false);
        return;
    }
    m_progressRow->setVisible(true);
    m_progressBar->setValue(static_cast<int>(percent));
    m_progressLabel->setText(QStringLiteral("%1%").arg(static_cast<int>(percent)));
}

void ModelCard::setSelected(bool selected) {
    if (m_selected == selected) return;
    m_selected = selected;
    setProperty("selected", selected);
    style()->unpolish(this);
    style()->polish(this);
}

void ModelCard::mousePressEvent(QMouseEvent *event) {
    Q_UNUSED(event);
    emit cardClicked(this);
    QFrame::mousePressEvent(event);
}
