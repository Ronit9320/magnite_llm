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
    explicit AdapterCard(const QJsonObject &data, QWidget *parent = nullptr);

    QString id() const { return m_id; }

    void setActive(bool active);

signals:
    void activateRequested(AdapterCard *card);
    void exportRequested(AdapterCard *card);
    void deleteRequested(AdapterCard *card);

private:
    QPushButton *m_activateButton;
    QLabel *m_activeLabel = nullptr;
    QString m_id;
    bool m_active = false;
};
