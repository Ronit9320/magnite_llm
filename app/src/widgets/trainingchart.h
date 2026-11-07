#pragma once

#include <QJsonArray>
#include <QWidget>

// Draws raw scalar samples at their actual optimiser steps. No synthetic points.
class TrainingChart : public QWidget {
    Q_OBJECT
public:
    explicit TrainingChart(const QString &metric, QWidget *parent = nullptr);
    void setHistory(const QJsonArray &history, int totalSteps = 0);
    int sampleCount() const;
    QSize minimumSizeHint() const override { return {180, 150}; }
protected:
    void paintEvent(QPaintEvent *) override;
    bool event(QEvent *event) override;
private:
    struct Sample { double step; double value; QString key; };
    QList<Sample> samples() const;
    QRectF plotRect() const;
    QString m_metric;
    QJsonArray m_history;
    int m_totalSteps = 0;
};
