#pragma once
#include <QFrame>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QMap>
#include <functional>
class QLabel;
class QGridLayout;
class QPushButton;
class QVBoxLayout;
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
    // Rows from /system/packages and the install job from /system/packages/install.
    void setPackages(const QJsonArray& rows);
    void setInstallJob(const QJsonObject& job);
    void setInstallHandlers(std::function<void(const QJsonObject& package)> install,
                            std::function<void()> cancel);
    // Seams for tests and accessibility; null / empty when the card or control is absent.
    QString packageStatus(const QString& id) const;
    QString packageNote(const QString& id) const;
    QPushButton* installButton(const QString& id) const;
    QPushButton* cancelButton() const;

  private:
    void renderPackages();
    QLabel *m_status, *m_application, *m_backend, *m_system, *m_python, *m_device, *m_memory;
    QVBoxLayout* m_packages;
    QJsonArray m_rows;
    QJsonObject m_job;
    QMap<QString, QFrame*> m_cards;
    std::function<void(const QJsonObject&)> m_install;
    std::function<void()> m_cancel;
};
