#pragma once
#include <QFrame>
#include <QJsonObject>
class QLabel;
class StorageDistribution;

class StorageSummary : public QFrame {
  public:
    explicit StorageSummary(QWidget* parent = nullptr);
    void setMeasurements(const QJsonObject& data);
    void setUnavailable(const QString& message);

  private:
    QLabel *m_total, *m_caption, *m_models, *m_workspace, *m_free;
    StorageDistribution* m_distribution;
};

class DiagnosticsSummary : public QFrame {
  public:
    explicit DiagnosticsSummary(QWidget* parent = nullptr);
    void setDetails(const QJsonObject& data);
    void setUnavailable(const QString& message);

  private:
    QLabel *m_status, *m_application, *m_backend, *m_system, *m_python, *m_device, *m_memory;
    QList<QLabel*> m_engines;
};
