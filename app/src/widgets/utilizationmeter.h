#pragma once

#include <QWidget>

class QFrame;
class QJsonObject;

class UtilizationMeter : public QWidget {
public:
    UtilizationMeter(int segmentHeight, int spacing, QWidget *parent = nullptr);

    void applyTelemetry(const QJsonObject &data);
    QString label() const;
    QString reading() const;
    int litSegments() const { return m_litSegments; }

private:
    QList<QFrame *> m_segments;
    QString m_reading = QStringLiteral("—");
    int m_litSegments = 0;
    bool m_showCpu = false;
};
