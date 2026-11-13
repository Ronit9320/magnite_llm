#include "widgets/settingssummary.h"
#include "theme.h"
#include "widgets/button.h"
#include <QCoreApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>

namespace {
QLabel* text(const QString& value, const char* name) {
    auto* label = new QLabel(value);
    label->setObjectName(name);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    label->setMinimumWidth(0);
    return label;
}
QFrame* metricCard(const QString& title, QLabel** value) {
    auto* card = new QFrame;
    card->setObjectName("settingsMetric");
    auto* body = new QVBoxLayout(card);
    body->setContentsMargins(14, 12, 14, 12);
    body->setSpacing(7);
    body->addWidget(text(title, "settingsHint"));
    *value = text("—", "settingsMetricValue");
    body->addWidget(*value);
    return card;
}
QString fileSizeText(const QJsonValue& value) {
    return value.isDouble() ? QLocale().formattedDataSize(qint64(value.toDouble()))
                            : QString("Unknown");
}
} // namespace
class StorageDistribution : public QWidget {
  public:
    explicit StorageDistribution(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName("settingsStorageDistribution");
        setFixedHeight(8);
        setAccessibleName("Storage distribution");
    }
    // models, workspace, other used, free. Without disk capacity the last two are zero and the
    // bar divides only the measured files between models and workspace.
    void setSegments(const QList<double>& segments) {
        m_segments = segments;
        update();
    }
    QList<double> segments() const { return m_segments; }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QPainterPath clip;
        clip.addRoundedRect(QRectF(rect()), 4, 4);
        p.setClipPath(clip);
        // The free portion is the bare track, so the bar reads as the whole disk.
        p.fillRect(rect(), QColor(Theme::meterTrack()));
        double total = 0;
        for (double value : m_segments)
            total += value;
        if (total <= 0)
            return;
        const QColor colors[3] = {QColor(Theme::accentPrimary()), QColor(Theme::comparisonSeries()),
                                  QColor(Theme::textMuted())};
        qreal x = 0;
        for (int i = 0; i < 3 && i < m_segments.size(); ++i) {
            if (m_segments[i] <= 0)
                continue;
            // A measured but tiny share stays visible instead of vanishing below one pixel.
            const qreal w = qMax<qreal>(3, width() * m_segments[i] / total);
            p.fillRect(QRectF(x, 0, w, height()), colors[i]);
            x += w;
        }
    }

  private:
    QList<double> m_segments{0, 0, 0, 0};
};
StorageSummary::StorageSummary(QWidget* parent) : QFrame(parent) {
    setObjectName("settingsStorageOverview");
    auto* body = new QVBoxLayout(this);
    body->setContentsMargins(18, 18, 18, 16);
    body->setSpacing(12);
    body->addWidget(text("MEASURED STORAGE", "settingsEyebrow"));
    m_total = text("—", "settingsStorageTotal");
    body->addWidget(m_total);
    m_caption = text("Connect to the local service to measure stored files.", "settingsHint");
    body->addWidget(m_caption);
    m_distribution = new StorageDistribution;
    body->addWidget(m_distribution);
    auto* metrics = new QGridLayout;
    m_metrics = metrics;
    metrics->setSpacing(10);
    auto* models = metricCard("●  Shared model cache", &m_models);
    models->setProperty("metricTone", "models");
    auto* workspace = metricCard("●  Workspace", &m_workspace);
    workspace->setProperty("metricTone", "workspace");
    m_otherCard = metricCard("●  Other used", &m_other);
    m_otherCard->setProperty("metricTone", "other");
    m_otherTitle = m_otherCard->findChildren<QLabel*>().first();
    m_otherCard->setVisible(false);
    metrics->addWidget(models, 0, 0);
    metrics->addWidget(workspace, 0, 1);
    metrics->addWidget(m_otherCard, 0, 2);
    metrics->setColumnStretch(0, 1);
    metrics->setColumnStretch(1, 1);
    metrics->setColumnStretch(2, 0);
    body->addLayout(metrics);
    auto* free = new QHBoxLayout;
    free->addWidget(text("Free on data volume", "settingsHint"), 1);
    m_free = text("Unknown", "settingsStorageFree");
    m_free->setWordWrap(false); // "X of Y" stays on one line; the hint beside it can wrap instead
    free->addWidget(m_free);
    body->addLayout(free);
}
QList<double> StorageSummary::segments() const { return m_distribution->segments(); }
QString StorageSummary::freeText() const { return m_free->text(); }
QString StorageSummary::otherText() const { return m_other->text(); }
QString StorageSummary::otherTitle() const { return m_otherTitle->text(); }
bool StorageSummary::otherHidden() const { return m_otherCard->isHidden(); }
void StorageSummary::setUnavailable(const QString& message) {
    m_caption->setText(message);
    m_total->setText("—");
    m_models->setText("Unknown");
    m_workspace->setText("Unknown");
    m_other->setText("Unknown");
    m_otherCard->setVisible(false);
    m_metrics->setColumnStretch(2, 0);
    m_free->setText("Unknown");
    m_distribution->setSegments({0, 0, 0, 0});
}
void StorageSummary::setMeasurements(const QJsonObject& data) {
    const auto workspace = data["workspace"].toObject(), models = data["models"].toObject();
    const bool combined = data["combined_locations"].toBool();
    const bool complete =
        workspace["complete"].toBool() && (combined || models["complete"].toBool());
    const double workspaceBytes = workspace["bytes"].toDouble(),
                 modelBytes = combined ? 0 : models["bytes"].toDouble();
    m_total->setText((complete ? QString() : QString("At least ")) +
                     fileSizeText(workspaceBytes + modelBytes));
    m_caption->setText(combined ? "App data and model cache share one location."
                                : "Across your workspace and shared model cache.");
    auto measured = [](const QJsonObject& part) {
        return (part["complete"].toBool() ? QString() : QString("≥ ")) +
               fileSizeText(part["bytes"]);
    };
    m_models->setText(combined ? "Included in workspace" : measured(models));
    m_workspace->setText(measured(workspace));

    const bool freeKnown = data["free_bytes"].isDouble();
    const bool capacity = freeKnown && data["disk_total_bytes"].toDouble() > 0;
    QList<double> segments{modelBytes, workspaceBytes, 0, 0};
    if (capacity) {
        const double total = data["disk_total_bytes"].toDouble(), free = data["free_bytes"].toDouble();
        // Capacity is that of the data volume; a cache on another disk does not use it.
        const double modelsOnDisk = data["cache_on_data_volume"].toBool(true) ? modelBytes : 0;
        const double other = qMax(0.0, total - free - modelsOnDisk - workspaceBytes);
        segments = {modelsOnDisk, workspaceBytes, other, free};
        // What is left over also covers anything the bounded scan did not reach.
        m_otherTitle->setText(complete ? "●  Other used" : "●  Other & unscanned");
        m_other->setText((complete ? QString() : QString("≤ ")) + fileSizeText(other));
        m_free->setText(fileSizeText(free) + " of " + fileSizeText(total));
    } else {
        m_free->setText(freeKnown ? fileSizeText(data["free_bytes"]) : QString("Unknown"));
    }
    m_otherCard->setVisible(capacity);
    m_metrics->setColumnStretch(2, capacity ? 1 : 0); // no empty third column without capacity
    m_distribution->setSegments(segments);
    m_distribution->setAccessibleDescription(
        capacity ? "Models: " + m_models->text() + ". Workspace: " + m_workspace->text() +
                       ". Other: " + m_other->text() + ". Free: " + m_free->text() + "."
                 : "Model cache: " + m_models->text() + ". Workspace: " + m_workspace->text() +
                       ". File-size distribution across measured locations, not disk capacity.");
    setAccessibleDescription(
        m_total->text() + " measured files. " +
        (complete ? "Scan complete." : "Partial scan; values are lower bounds."));
}
DiagnosticsSummary::DiagnosticsSummary(QWidget* parent) : QFrame(parent) {
    setObjectName("settingsDiagnosticsOverview");
    auto* body = new QVBoxLayout(this);
    body->setContentsMargins(0, 0, 0, 0);
    body->setSpacing(14);
    auto* header = new QHBoxLayout;
    header->addWidget(text("LOCAL RUNTIME", "settingsEyebrow"), 1);
    m_status = text("Disconnected", "settingsRuntimeStatus");
    header->addWidget(m_status);
    body->addLayout(header);
    auto* grid = new QGridLayout;
    grid->setSpacing(10);
    grid->addWidget(metricCard("Application", &m_application), 0, 0);
    grid->addWidget(metricCard("Backend", &m_backend), 0, 1);
    grid->addWidget(metricCard("System", &m_system), 1, 0);
    grid->addWidget(metricCard("Python", &m_python), 1, 1);
    grid->setColumnStretch(0, 1);
    grid->setColumnStretch(1, 1);
    body->addLayout(grid);
    auto* device = new QFrame;
    device->setObjectName("settingsMetric");
    auto* deviceBody = new QVBoxLayout(device);
    deviceBody->setContentsMargins(14, 12, 14, 12);
    deviceBody->setSpacing(7);
    deviceBody->addWidget(text("Device & memory", "settingsHint"));
    m_device = text("Unknown", "settingsMetricValue");
    m_memory = text("Measurements unavailable", "settingsHint");
    deviceBody->addWidget(m_device);
    deviceBody->addWidget(m_memory);
    body->addWidget(device);
    body->addWidget(text("Runtime packages", "settingsRowTitle"));
    auto* packages = new QWidget;
    m_packages = new QVBoxLayout(packages);
    m_packages->setContentsMargins(0, 0, 0, 0);
    m_packages->setSpacing(8);
    body->addWidget(packages);
    body->addWidget(text("Installed packages do not guarantee hardware or model compatibility. "
                         "Installs go to Magnite Space's own packages folder; the app itself is never modified.",
                         "settingsHint"));
    setPackages({});
    m_application->setText(QCoreApplication::applicationVersion());
}
void DiagnosticsSummary::setUnavailable(const QString& message) {
    m_status->setText("Disconnected");
    m_status->setAccessibleDescription(message);
    m_status->setProperty("connected", false);
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
    m_backend->setText("Unavailable");
    m_system->setText("Unknown");
    m_python->setText("Unknown");
    m_device->setText("Not measured");
    m_memory->setText(message);
    m_rows = {};
    m_job = {};
    renderPackages();
}
void DiagnosticsSummary::setDetails(const QJsonObject& data) {
    m_status->setText("● Connected");
    m_status->setProperty("connected", true);
    m_status->style()->unpolish(m_status);
    m_status->style()->polish(m_status);
    m_status->setAccessibleDescription("Local backend connected");
    m_backend->setText(data["version"].toString("Unknown"));
    m_system->setText(data["system"].toString() + " · " + data["architecture"].toString());
    m_python->setText(data["python"].toString("Unknown"));
    const auto device = data["device"].toObject();
    m_device->setText(device["device"].toString("Unknown"));
    m_memory->setText(fileSizeText(device["total_bytes"]) + " · " +
                      device["kind"].toString("Memory") + " · " +
                      device["accelerator"].toString("Unknown accelerator"));
}
void DiagnosticsSummary::setInstallHandlers(std::function<void(const QJsonObject&)> install,
                                            std::function<void()> cancel) {
    m_install = std::move(install);
    m_cancel = std::move(cancel);
}
void DiagnosticsSummary::setPackages(const QJsonArray& rows) {
    m_rows = rows;
    renderPackages();
}
void DiagnosticsSummary::setInstallJob(const QJsonObject& job) {
    m_job = job;
    renderPackages();
}
QString DiagnosticsSummary::packageStatus(const QString& id) const {
    auto* card = m_cards.value(id);
    auto* status = card ? card->findChild<QLabel*>("settingsPackageStatus") : nullptr;
    return status ? status->text() : QString();
}
QString DiagnosticsSummary::packageNote(const QString& id) const {
    auto* card = m_cards.value(id);
    auto* note = card ? card->findChild<QLabel*>("settingsPackageNote") : nullptr;
    return note ? note->text() : QString();
}
static QPushButton* packageButton(QFrame* card, const char* kind) {
    if (!card)
        return nullptr;
    for (auto* button : card->findChildren<QPushButton*>())
        if (button->property("packageButton").toString() == kind)
            return button;
    return nullptr;
}
QPushButton* DiagnosticsSummary::installButton(const QString& id) const {
    return packageButton(m_cards.value(id), "install");
}
QPushButton* DiagnosticsSummary::cancelButton() const {
    for (auto* card : m_cards)
        if (auto* cancel = packageButton(card, "cancel"))
            return cancel;
    return nullptr;
}
void DiagnosticsSummary::renderPackages() {
    m_cards.clear();
    while (auto* item = m_packages->takeAt(0)) {
        if (auto* old = item->widget()) {
            old->hide();
            old->setParent(nullptr);
            old->deleteLater();
        }
        delete item;
    }
    if (m_rows.isEmpty()) {
        m_packages->addWidget(text("Package status appears once the local backend is connected.", "settingsHint"));
        return;
    }
    const bool installing = m_job["state"].toString() == "running";
    for (const auto& value : m_rows) {
        const auto row = value.toObject();
        const QString id = row["id"].toString(), state = row["state"].toString();
        const bool thisJob = m_job["id"].toString() == id;
        const QString jobState = thisJob ? m_job["state"].toString() : QString();
        QString status, tone = "muted", note;
        if (jobState == "running") {
            status = "Installing…";
            tone = "installing";
        } else if (state == "installed") {
            status = "Installed · " + row["installed_version"].toString("unknown version");
            tone = "installed";
        } else if (jobState == "failed") {
            status = "Install failed";
            tone = "failed";
            note = m_job["log_tail"].toString().trimmed();
        } else if (state == "missing") {
            status = "Not installed";
            note = row["installable"].toBool() ? row["size_hint"].toString() : row["reason"].toString();
            if (jobState == "cancelled")
                note = "Install cancelled. " + note;
        } else {
            status = row["reason"].toString("Unavailable");
        }
        auto* card = new QFrame;
        card->setObjectName("settingsPackageCard");
        card->setProperty("packageId", id);
        auto* body = new QVBoxLayout(card);
        body->setContentsMargins(14, 11, 14, 11);
        body->setSpacing(5);
        auto* head = new QHBoxLayout;
        head->addWidget(text(row["label"].toString(id), "settingsRowTitle"), 1);
        auto* pill = text(status, "settingsPackageStatus");
        pill->setWordWrap(false); // a status such as "Not available on macOS" stays on one line
        pill->setProperty("packageState", tone);
        head->addWidget(pill);
        body->addLayout(head);
        body->addWidget(text(row["role"].toString(), "settingsHint"));
        if (!note.isEmpty())
            body->addWidget(text(note, "settingsPackageNote"));
        if (jobState == "running") {
            auto* bar = new QProgressBar;
            bar->setRange(0, 0); // pip reports no total, so the bar stays indeterminate
            bar->setTextVisible(false);
            bar->setFixedHeight(8);
            body->addWidget(bar);
            auto* cancel = new Button("Cancel");
            cancel->setObjectName("secondaryBtn");
            cancel->setProperty("packageButton", "cancel");
            cancel->setAccessibleName("Cancel installing " + row["label"].toString(id));
            QObject::connect(cancel, &QPushButton::clicked, card, [this] { if (m_cancel) m_cancel(); });
            body->addWidget(cancel, 0, Qt::AlignRight);
        } else if (state == "missing" && row["installable"].toBool()) {
            auto* install = new Button(jobState == "failed" ? "Retry" : "Install");
            install->setObjectName("secondaryBtn");
            install->setProperty("packageButton", "install");
            install->setAccessibleName("Install " + row["label"].toString(id));
            install->setEnabled(!installing);
            if (installing)
                install->setToolTip("Another install is in progress.");
            QObject::connect(install, &QPushButton::clicked, card, [this, row] { if (m_install) m_install(row); });
            body->addWidget(install, 0, Qt::AlignRight);
        }
        m_cards.insert(id, card);
        m_packages->addWidget(card);
    }
}
