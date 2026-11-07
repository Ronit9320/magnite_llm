#pragma once

#include <QJsonArray>
#include <QWidget>

// Draws raw scalar samples at their actual optimiser steps. No synthetic points.
class QVariantAnimation;

class TrainingChart : public QWidget {
    Q_OBJECT
public:
    explicit TrainingChart(const QString &metric, QWidget *parent = nullptr);
    void setHistory(const QJsonArray &history, int totalSteps = 0);
    int sampleCount() const;
    bool isAnimating() const;
    QSize minimumSizeHint() const override { return {180, 150}; }
protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    bool event(QEvent *event) override;
private:
    struct Sample { double step; double value; QString key; };
    QList<Sample> samples() const;
    QRectF plotRect() const;
    QString m_metric;
    QJsonArray m_history;
    int m_totalSteps = 0;
    QVariantAnimation *m_animation;
    double m_reveal = 1.0;
    double m_fromStep = 0;
};
