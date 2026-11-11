#pragma once

#include <QJsonArray>
#include <QWidget>
#include <QPointer>

// Draws raw scalar samples at their actual optimiser steps. No synthetic points.
class QVariantAnimation;
class QLabel;
class Button;
class QDialog;

class TrainingChart : public QWidget {
    Q_OBJECT
public:
    explicit TrainingChart(const QString &metric, QWidget *parent = nullptr);
    void setHistory(const QJsonArray &history, int totalSteps = 0);
    void setComparison(const QJsonArray &first, const QJsonArray &second, int totalSteps = 0);
    void setSmoothing(bool enabled);
    int sampleCount() const;
    bool isAnimating() const;
    QSize minimumSizeHint() const override { return {180, 150}; }
protected:
    void paintEvent(QPaintEvent *) override;
    void showEvent(QShowEvent *) override;
    void hideEvent(QHideEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    bool event(QEvent *event) override;
private:
    struct Sample { double step; double value; QString key; };
    QList<Sample> samples() const;
    QRectF plotRect() const;
    void updateHover();
    void expand();
    void syncExpanded();
    static QPair<double, double> stepRange(const QList<Sample> &values);
    Button *m_expandButton = nullptr;
    QPointer<QDialog> m_expandedDialog;
    QPointer<TrainingChart> m_expandedChart;
    QString m_metric;
    QJsonArray m_history;
    int m_totalSteps = 0;
    QVariantAnimation *m_animation;
    double m_reveal = 1.0;
    double m_fromStep = 0;
    bool m_comparison = false;
    bool m_smoothed = false;
    double m_hoverX = -1;
    QPointF m_hoverPosition{-1,-1};
    QLabel *m_hoverLabel = nullptr;
};
