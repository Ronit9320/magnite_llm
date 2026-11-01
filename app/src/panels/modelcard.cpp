#include "panels/modelcard.h"

#include "theme.h"

#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMouseEvent>
#include <QKeyEvent>
#include <QLocale>
#include <QProgressBar>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

ModelCard::ModelCard(const QJsonObject &data, bool isHub, QWidget *parent)
    : QFrame(parent)
    , m_id(isHub ? data.value("repo_id").toString() : data.value("id").toString())
    , m_data(data)
    , m_downloaded(isHub && data.value("downloaded").toBool())
    , m_isHub(isHub)
    , m_progressRow(nullptr)
    , m_progressLabel(nullptr)
    , m_progressBar(nullptr)
{
    setObjectName(QStringLiteral("modelCard"));
    setMinimumWidth(230);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    setCursor(Qt::PointingHandCursor);
    setFocusPolicy(Qt::StrongFocus);
    setAccessibleName(m_id);
    setAccessibleDescription("Press Space to select");

    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 12, 12, 12);
    layout->setSpacing(8);

    // Row 1: name (Space Grotesk) + architecture pill.
    auto *nameRow = new QHBoxLayout();
    nameRow->setSpacing(8);
    auto *name = new QLabel(m_id, this);
    name->setObjectName(QStringLiteral("cardTitle"));
    name->setWordWrap(true);
    name->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);
    name->setToolTip(m_id);
    nameRow->addWidget(name, 1);
    if (!isHub) {
        const QString arch = data.value("architecture").toString();
        if (!arch.isEmpty()) {
            auto *badge = new QLabel(arch, this);
            badge->setObjectName(QStringLiteral("archBadge"));
            badge->setWordWrap(true);
            badge->setMaximumWidth(110);
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
        auto *size = new QLabel(formatSizeMb(data.value("size_mb").toDouble()), this);
        size->setObjectName(QStringLiteral("cardMeta"));
        metaRow->addWidget(size);
    }
    metaRow->addStretch(1);
    layout->addLayout(metaRow);

    // Row 3: license on both views; downloads/likes on Hub hits.
    auto *footRow = new QHBoxLayout();
    footRow->setSpacing(6);
    const QString license = data.value("license").toString();
    auto *licenseLabel = new QLabel(license.isEmpty() ? QStringLiteral("License not specified") : license, this);
    licenseLabel->setObjectName(QStringLiteral("cardMeta"));
    footRow->addWidget(licenseLabel);
    if (isHub) {
        m_statsBase = QStringLiteral("%1 downloads · %2 likes")
                          .arg(data.value("downloads").toInt())
                          .arg(data.value("likes").toInt());
        m_statsLabel = new QLabel(m_statsBase, this);
        m_statsLabel->setObjectName(QStringLiteral("cardMeta"));
        m_statsLabel->setWordWrap(true);   // the size can make this long on a narrow card
        footRow->addWidget(m_statsLabel, 1);   // takes the spare width so it only wraps when it must
    } else {
        footRow->addStretch(1);
    }
    layout->addLayout(footRow);

    licenseLabel->setWordWrap(true);
    appendProgressRow();
    auto *actions = new QHBoxLayout();
    // Hub hits already in the library offer to load them instead of downloading again.
    const QString stateText = !isHub ? QStringLiteral("✓ Ready locally")
        : m_downloaded ? QStringLiteral("✓ Downloaded") : QStringLiteral("Available on the Hub");
    auto *state = new QLabel(stateText, this);
    state->setObjectName(m_downloaded ? "cardDownloaded" : "cardMeta");
    const bool loads = !isHub || m_downloaded;
    auto *action = new QPushButton(loads ? "Load model" : "Download",this);
    action->setObjectName("primaryBtn");
    m_actionButton = action;
    if (isHub) {
        actions->addWidget(state,1);
    } else {
        // Local cards have a second action; the status gets its own line so a
        // narrow card never has to squeeze three things into one row.
        layout->addWidget(state);
        actions->addStretch(1);
        m_deleteButton = new QPushButton("Delete",this);
        m_deleteButton->setObjectName("cardDanger");
        m_deleteButton->setCursor(Qt::PointingHandCursor);
        m_deleteButton->setAccessibleName("Delete model "+m_id);
        actions->addWidget(m_deleteButton);
    }
    actions->addWidget(action);
    layout->addLayout(actions);
    action->setAccessibleName((loads ? "Load " : "Download ")+m_id);
    for (auto *label : findChildren<QLabel *>()) {
        label->setTextFormat(Qt::PlainText);
        label->setAttribute(Qt::WA_TransparentForMouseEvents);
    }
    connect(action,&QPushButton::clicked,this,[this]() { emit actionRequested(this); });
    if (m_deleteButton)
        connect(m_deleteButton,&QPushButton::clicked,this,[this]() { emit deleteRequested(this); });
}

QString ModelCard::localId() const {
    if (!m_isHub) return m_id;
    QString ref = m_id;
    return QStringLiteral("huggingface:") + ref.replace('/', QStringLiteral("--"));
}

void ModelCard::appendProgressRow() {
    // Bar on its own line, then "size · percent" on the left and Cancel on the right,
    // so the text never squeezes the bar on a narrow card.
    auto *column = new QVBoxLayout();
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(6);

    m_progressBar = new QProgressBar(this);
    m_progressBar->setObjectName(QStringLiteral("cardProgress"));
    m_progressBar->setRange(0, 100);
    m_progressBar->setValue(0);
    m_progressBar->setTextVisible(false);
    m_progressBar->setFixedHeight(4);
    column->addWidget(m_progressBar);

    auto *detailRow = new QHBoxLayout();
    detailRow->setSpacing(8);
    m_progressLabel = new QLabel("0%", this);
    m_progressLabel->setObjectName(QStringLiteral("monoLabel"));
    detailRow->addWidget(m_progressLabel, 1);

    m_cancelButton = new QPushButton("Cancel", this);
    m_cancelButton->setObjectName(QStringLiteral("secondaryBtn"));
    m_cancelButton->setAccessibleName("Cancel download " + m_id);
    m_cancelButton->setCursor(Qt::PointingHandCursor);
    detailRow->addWidget(m_cancelButton);
    column->addLayout(detailRow);
    connect(m_cancelButton, &QPushButton::clicked, this, [this]() { emit cancelRequested(this); });

    m_progressRow = new QWidget(this);
    m_progressRow->setLayout(column);
    (static_cast<QVBoxLayout *>(layout()))->addWidget(m_progressRow);
    m_progressRow->setVisible(false);
}

QString ModelCard::formatSizeMb(double megabytes) {
    if (megabytes <= 0) return QStringLiteral("-");
    if (megabytes >= 1024) return QStringLiteral("%1 GB").arg(megabytes / 1024.0, 0, 'f', 1);
    return QStringLiteral("%1 MB").arg(megabytes, 0, 'f', megabytes < 10 ? 1 : 0);
}

void ModelCard::setSize(qint64 bytes) {
    if (!m_statsLabel) return;   // local cards already show their own size
    m_statsLabel->setText(m_statsBase + QStringLiteral(" · ") +
                          (bytes > 0 ? formatBytes(bytes) : QStringLiteral("size unknown")));
}

void ModelCard::setDeleteEnabled(bool enabled) {
    if (m_deleteButton) m_deleteButton->setEnabled(enabled);
}

QString ModelCard::formatBytes(qint64 bytes) {
    return QLocale().formattedDataSize(bytes < 0 ? 0 : bytes, 1, QLocale::DataSizeSIFormat);
}

void ModelCard::setProgress(double percent, qint64 bytesDone, qint64 bytesTotal) {
    if (!m_progressRow) return;
    // While this card is downloading its main action would only repeat the download.
    if (m_actionButton) m_actionButton->setEnabled(percent < 0);
    if (percent < 0) {
        m_progressRow->setVisible(false);
        setCancelling(false);
        return;
    }
    m_progressRow->setVisible(true);
    m_progressBar->setValue(static_cast<int>(percent));
    const QString pct = QStringLiteral("%1%").arg(static_cast<int>(percent));
    m_progressLabel->setText(bytesTotal > 0
        ? QStringLiteral("%1 / %2 · %3").arg(formatBytes(bytesDone), formatBytes(bytesTotal), pct)
        : pct);
}

void ModelCard::setCancelling(bool cancelling) {
    if (!m_cancelButton) return;
    m_cancelButton->setEnabled(!cancelling);
    m_cancelButton->setText(cancelling ? QStringLiteral("Cancelling…") : QStringLiteral("Cancel"));
}

void ModelCard::setSelected(bool selected) {
    if (m_selected == selected) return;
    m_selected = selected;
    setProperty("selected", selected);
    setAccessibleDescription(selected ? "Selected. Press Space to select." : "Press Space to select.");
    style()->unpolish(this);
    style()->polish(this);
    update();
}

void ModelCard::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        setFocus(Qt::MouseFocusReason);
        emit cardClicked(this);
        event->accept();
        return;
    }
    QFrame::mousePressEvent(event);
}

void ModelCard::keyPressEvent(QKeyEvent *event) {
    if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (!event->isAutoRepeat()) emit cardClicked(this);
        event->accept();
        return;
    }
    QFrame::keyPressEvent(event);
}
