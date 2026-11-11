#pragma once
#include <QFrame>
#include <QJsonObject>
#include <QList>
class QLabel;
class QGridLayout;
class StorageDistribution;

class StorageSummary : public QFrame {
  public:
    explicit StorageSummary(QWidget* parent = nullptr);
    void setMeasurements(const QJsonObject& data);
    void setUnavailable(const QString& message);
    // Bar segments in bytes: models, workspace, other used, free (test and accessibility seam).
    QList<double> segments() const;
    QString freeText() const;
    QString otherText() const;
    QString otherTitle() const;
    bool otherHidden() const;

  private:
    QLabel *m_total, *m_caption, *m_models, *m_workspace, *m_other, *m_otherTitle, *m_free;
    QFrame* m_otherCard;
    QGridLayout* m_metrics;
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
