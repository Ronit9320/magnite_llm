#include "trainingchart.h"
#include "theme.h"

#include <QHelpEvent>
#include <QVariantAnimation>
#include <QSettings>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>
#include <algorithm>
#include <cmath>
#include <limits>

TrainingChart::TrainingChart(const QString &metric, QWidget *parent)
    : QWidget(parent), m_metric(metric), m_animation(new QVariantAnimation(this)) {
    m_animation->setDuration(550);
    m_animation->setStartValue(0.0); m_animation->setEndValue(1.0);
    m_animation->setEasingCurve(QEasingCurve::OutCubic);
    connect(m_animation,&QVariantAnimation::valueChanged,this,[this](const QVariant &value) {
        m_reveal=value.toDouble();
        if (QSettings().value("appearance/studio3ReducedMotion",false).toBool()) {
            m_animation->stop(); m_reveal=1.0;
        }
        update();
    });
    setMinimumHeight(150);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAccessibleName(metric == "loss" ? "Training and validation loss" :
                      metric == "learning_rate" ? "Learning rate" : "Gradient norm");
    setToolTip("Hover over a sample to inspect its measured value.");
}

void TrainingChart::setHistory(const QJsonArray &history, int totalSteps) {
    if (m_history == history && m_totalSteps == qMax(0,totalSteps)) return;
    const auto previous=samples();
    m_fromStep=previous.isEmpty() ? 0 : previous.last().step;
    m_animation->stop();
    m_history = history;
    m_totalSteps = qMax(0, totalSteps);
    const auto values = samples();
    setAccessibleDescription(values.isEmpty() ? "Waiting for measured samples." :
        QString("%1 measured samples. Latest: step %2, %3 = %4.")
            .arg(values.size()).arg(values.last().step).arg(values.last().key)
            .arg(values.last().value, 0, 'g', 6));
    m_reveal=1.0;
    if (!values.isEmpty() && isVisible() && !QSettings().value("appearance/studio3ReducedMotion",false).toBool())
        { m_reveal=0.0; m_animation->start(); }
    update();
}

bool TrainingChart::isAnimating() const { return m_animation->state()==QAbstractAnimation::Running; }
void TrainingChart::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    if (sampleCount()>0 && !QSettings().value("appearance/studio3ReducedMotion",false).toBool()) {
        m_fromStep=0; m_reveal=0.0; m_animation->start();
    }
}
void TrainingChart::hideEvent(QHideEvent *event) {
    m_animation->stop(); m_reveal=1.0;
    QWidget::hideEvent(event);
}

QList<TrainingChart::Sample> TrainingChart::samples() const {
    QList<Sample> result;
    for (const auto &value : m_history) {
        const auto row = value.toObject();
        const auto step = row.value("step");
        if (!step.isDouble() || !std::isfinite(step.toDouble()) || step.toDouble() < 0) continue;
        for (const QString &key : {m_metric, m_metric == "loss" ? QString("eval_loss") : QString()}) {
            if (key.isEmpty()) continue;
            const auto v = row.value(key);
            if (v.isDouble() && std::isfinite(v.toDouble()))
                result.append({step.toDouble(), v.toDouble(), key});
        }
    }
    std::stable_sort(result.begin(), result.end(), [](const Sample &a, const Sample &b) { return a.step < b.step; });
    return result;
}

int TrainingChart::sampleCount() const { return samples().size(); }
QRectF TrainingChart::plotRect() const { return QRectF(58, 12, qMax(1, width()-74), qMax(1, height()-42)); }

void TrainingChart::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto area = plotRect();
    const auto values = samples();
    p.setFont(QFont(Theme::bodyFont(), 9));
    p.setPen(QColor(Theme::surfaceBorder()));
    for (int i=0; i<3; ++i) {
        const double y = area.top()+area.height()*i/2;
        p.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
    }
    p.setPen(QColor(Theme::textMuted()));
    if (values.isEmpty()) {
        p.drawText(area, Qt::AlignCenter, "Waiting for measured samples");
        return;
    }
    double low=std::numeric_limits<double>::max(), high=std::numeric_limits<double>::lowest();
    double maxStep=qMax(1, m_totalSteps);
    for (const auto &s : values) { low=qMin(low,s.value); high=qMax(high,s.value); maxStep=qMax(maxStep,s.step); }
    const double padding = high == low ? qMax(std::abs(high)*0.1, 0.000001) : (high-low)*0.1;
    low-=padding; high+=padding;
    auto point = [&](const Sample &s) { return QPointF(area.left()+s.step/maxStep*area.width(), area.bottom()-(s.value-low)/(high-low)*area.height()); };
    for (int i=0; i<3; ++i) {
        const double y = area.top()+area.height()*i/2;
        const double value=high-(high-low)*i/2;
        const bool scientific=value!=0 && (std::abs(value)<0.001 || std::abs(value)>=10000);
        p.drawText(QRectF(0,y-9,51,18), Qt::AlignRight|Qt::AlignVCenter,
                   QString::number(value,scientific?'e':'g',scientific?1:3));
    }
    p.drawText(QRectF(area.left(),area.bottom()+7,80,20),Qt::AlignLeft,"Step 0");
    p.drawText(QRectF(area.right()-90,area.bottom()+7,90,20),Qt::AlignRight,QString::number(maxStep,'g',6));
    double latest=values.last().step;
    const double revealStep=qMin(m_fromStep,latest)+(latest-qMin(m_fromStep,latest))*m_reveal;
    p.save();
    p.setOpacity(0.55+0.45*m_reveal);
    p.setClipRect(QRectF(area.left()-3,area.top()-3,area.width()*revealStep/maxStep+6,area.height()+6));
    for (const QString &key : {m_metric, m_metric == "loss" ? QString("eval_loss") : QString()}) {
        if (key.isEmpty()) continue;
        const QColor color(key == "eval_loss" ? Theme::statusSuccess() : Theme::accentLabel());
        p.setPen(QPen(color,2,key == "eval_loss" ? Qt::DashLine : Qt::SolidLine));
        QPainterPath path;
        bool first=true;
        for (const auto &s : values) if (s.key == key) {
            if (first) path.moveTo(point(s)); else path.lineTo(point(s));
            first=false;
        }
        p.drawPath(path);
        p.setBrush(color);
        for (const auto &s : values) if (s.key == key) p.drawEllipse(point(s),2,2);
        p.setBrush(Qt::NoBrush);
    }
    p.restore();
}

bool TrainingChart::event(QEvent *event) {
    if (event->type() == QEvent::ToolTip) {
        const auto values=samples();
        auto *help=static_cast<QHelpEvent *>(event);
        const auto area=plotRect();
        if (!values.isEmpty() && area.contains(help->pos())) {
            double maxStep=qMax(1,m_totalSteps);
            for (const auto &s : values) maxStep=qMax(maxStep,s.step);
            const double step=(help->pos().x()-area.left())/area.width()*maxStep;
            const auto nearest=std::min_element(values.begin(),values.end(),[step](const Sample &a,const Sample &b) {
                return std::abs(a.step-step)<std::abs(b.step-step);
            });
            QStringList lines{QString("Step %1").arg(nearest->step)};
            for (const auto &s : values) if (s.step == nearest->step)
                lines << QString("%1: %2").arg(s.key).arg(s.value,0,'g',7);
            QToolTip::showText(help->globalPos(),lines.join('\n'),this);
        } else QToolTip::hideText();
        return true;
    }
    return QWidget::event(event);
}
