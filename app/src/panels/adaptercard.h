#pragma once

#include <QFrame>

#include <QJsonObject>
#include <QString>

class QPushButton;
class QLabel;

// Adapter registry row with activation, export and deletion actions.
class AdapterCard : public QFrame {
    Q_OBJECT

public:
    explicit AdapterCard(const QJsonObject &data, QWidget *parent = nullptr, bool compact = false);

    QString id() const { return m_id; }

    const QJsonObject &data() const { return m_data; }

    void setActive(bool active);

signals:
    void activateRequested(AdapterCard *card);
    void runDetailsRequested(const QString &jobId);
    void exportRequested(AdapterCard *card);
    void deleteRequested(AdapterCard *card);

private:
    QPushButton *m_activateButton;
    QLabel *m_activeLabel = nullptr;
    QString m_id;
    QJsonObject m_data;
    bool m_active = false;
};
