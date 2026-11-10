#include "widgets/settingssummary.h"
#include "theme.h"
#include <QCoreApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLocale>
#include <QPainter>
#include <QPainterPath>
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
        setAccessibleName("Measured file-size distribution");
    }
    void setSizes(double models, double workspace) {
        m_models = models;
        m_workspace = workspace;
        update();
    }

  protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        QPainterPath clip;
        clip.addRoundedRect(QRectF(rect()), 4, 4);
        p.setClipPath(clip);
        p.fillRect(rect(), QColor(Theme::surfaceBorder()));
        const double total = m_models + m_workspace;
        if (total <= 0)
            return;
        const qreal split = width() * m_models / total;
        p.fillRect(QRectF(0, 0, split, height()), QColor(Theme::accentPrimary()));
        p.fillRect(QRectF(split, 0, width() - split, height()), QColor(Theme::comparisonSeries()));
    }

  private:
    double m_models = 0, m_workspace = 0;
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
    metrics->setSpacing(10);
    auto* models = metricCard("●  Shared model cache", &m_models);
    models->setProperty("metricTone", "models");
    auto* workspace = metricCard("●  Workspace", &m_workspace);
    workspace->setProperty("metricTone", "workspace");
    metrics->addWidget(models, 0, 0);
    metrics->addWidget(workspace, 0, 1);
    metrics->setColumnStretch(0, 1);
    metrics->setColumnStretch(1, 1);
    body->addLayout(metrics);
    auto* free = new QHBoxLayout;
    free->addWidget(text("Free on data volume", "settingsHint"), 1);
    m_free = text("Unknown", "settingsStorageFree");
    free->addWidget(m_free);
    body->addLayout(free);
}
void StorageSummary::setUnavailable(const QString& message) {
    m_caption->setText(message);
    m_total->setText("—");
    m_models->setText("Unknown");
    m_workspace->setText("Unknown");
    m_free->setText("Unknown");
    m_distribution->setSizes(0, 0);
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
    m_free->setText(fileSizeText(data["free_bytes"]));
    m_distribution->setSizes(modelBytes, workspaceBytes);
    m_distribution->setAccessibleDescription(
        "Model cache: " + m_models->text() + ". Workspace: " + m_workspace->text() +
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
    body->addWidget(text("Installed runtime packages", "settingsRowTitle"));
    for (const auto& engine : QStringList{"llama.cpp", "vLLM", "torch", "PEFT"}) {
        auto* host = new QFrame;
        host->setObjectName("settingsEngineRow");
        auto* row = new QHBoxLayout(host);
        row->setContentsMargins(12, 9, 12, 9);
        row->addWidget(text(engine, "settingsRowTitle"), 1);
        auto* version = text("Unknown", "settingsEngineStatus");
        row->addWidget(version);
        m_engines << version;
        body->addWidget(host);
    }
    body->addWidget(text("Installed packages do not guarantee hardware or model compatibility.",
                         "settingsHint"));
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
    for (auto* engine : m_engines)
        engine->setText("Unknown");
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
    const auto engines = data["engines"].toObject();
    const QStringList keys = {"llama.cpp", "vLLM", "torch", "PEFT"};
    for (int i = 0; i < keys.size(); ++i)
        m_engines[i]->setText(engines[keys[i]].isString()
                                  ? "Installed · " + engines[keys[i]].toString()
                                  : "Not installed");
}
