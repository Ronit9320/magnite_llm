#include "trainingchart.h"
#include "theme.h"

#include <QHelpEvent>
#include <QMouseEvent>
#include <QVariantAnimation>
#include <QSettings>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QLabel>
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
    setMouseTracking(true);
    setMinimumHeight(150);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    setAccessibleName(metric == "loss" ? "Training and validation loss" :
                      metric == "learning_rate" ? "Learning rate" : metric == "step_rate" ? "Measured throughput in steps per second" : "Gradient norm");
    m_hoverLabel=new QLabel(this); m_hoverLabel->setObjectName("trainingChartHover");
    m_hoverLabel->setAccessibleName("Measured chart values"); m_hoverLabel->setTextFormat(Qt::PlainText);
    m_hoverLabel->setAttribute(Qt::WA_TransparentForMouseEvents); m_hoverLabel->setFocusPolicy(Qt::NoFocus); m_hoverLabel->hide();
}

void TrainingChart::setHistory(const QJsonArray &history, int totalSteps) {
    if (m_comparison) { m_comparison=false; update(); }
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
    updateHover(); update();
}

bool TrainingChart::isAnimating() const { return m_animation->state()==QAbstractAnimation::Running; }
void TrainingChart::setComparison(const QJsonArray &first, const QJsonArray &second, int totalSteps) {
    QJsonArray combined=first;
    for (const auto &value : second) {
        const auto source=value.toObject();
        QJsonObject row{{"step",source["step"]}};
        for (const auto &key : {"loss","eval_loss","learning_rate","grad_norm","step_rate"})
            if (source.contains(key)) row["compare_"+QString(key)]=source[key];
        combined.append(row);
    }
    setHistory(combined,totalSteps);
    m_comparison=true;
    updateHover(); update();
}
void TrainingChart::showEvent(QShowEvent *event) {
    QWidget::showEvent(event);
    if (sampleCount()>0 && !QSettings().value("appearance/studio3ReducedMotion",false).toBool()) {
        m_fromStep=0; m_reveal=0.0; m_animation->start();
    }
}
void TrainingChart::hideEvent(QHideEvent *event) {
    m_animation->stop(); m_reveal=1.0; m_hoverX=-1; m_hoverPosition={-1,-1}; m_hoverLabel->hide();
    QWidget::hideEvent(event);
}

QList<TrainingChart::Sample> TrainingChart::samples() const {
    QList<Sample> result;
    for (const auto &value : m_history) {
        const auto row = value.toObject();
        const auto step = row.value("step");
        if (!step.isDouble() || !std::isfinite(step.toDouble()) || step.toDouble() < 0) continue;
        for (const QString &key : {m_metric, m_metric == "loss" ? QString("eval_loss") : QString(),
                                  "compare_"+m_metric, m_metric == "loss" ? QString("compare_eval_loss") : QString()}) {
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
QRectF TrainingChart::plotRect() const {
    QFont axisFont(Theme::monoFont()); axisFont.setPixelSize(9);
    const int left=qMax(42,QFontMetrics(axisFont).horizontalAdvance("-0.0e+00")+8);
    return QRectF(left,15,qMax(1,width()-left-24),qMax(1,height()-47));
}

void TrainingChart::paintEvent(QPaintEvent *) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const auto area = plotRect();
    const auto values = samples();
    QFont font(Theme::monoFont()); font.setPixelSize(9); p.setFont(font);
    QColor grid(Theme::textMuted()); grid.setAlphaF(0.13); p.setPen(grid);
    for (int i=0; i<5; ++i) {
        const double y = area.top()+area.height()*i/4;
        p.drawLine(QPointF(area.left(), y), QPointF(area.right(), y));
    }
    p.setPen(QColor(Theme::textMuted()));
    if (values.isEmpty()) {
        p.drawText(area, Qt::AlignCenter, "Waiting for measured samples");
        return;
    }
    double low=std::numeric_limits<double>::max(), high=std::numeric_limits<double>::lowest();
    double maxStep=1;
    for (const auto &s : values) { low=qMin(low,s.value); high=qMax(high,s.value); maxStep=qMax(maxStep,s.step); }
    const double padding = high == low ? qMax(std::abs(high)*0.1, 0.000001) : (high-low)*0.1;
    low=qMin(0.0,low-padding); high+=padding;
    if (high>0) {
        const double unit=std::pow(10,std::floor(std::log10(high)))/2;
        const double maxValue=high-padding;
        high=std::ceil(maxValue/unit)*unit;
        if (high<=low) high=low+padding;
    }
    auto point = [&](const Sample &s) { return QPointF(area.left()+s.step/maxStep*area.width(), area.bottom()-(s.value-low)/(high-low)*area.height()); };
    for (int i=0; i<5; ++i) {
        const double y = area.top()+area.height()*i/4;
        const double value=high-(high-low)*i/4;
        const bool scientific=value!=0 && (std::abs(value)<0.001 || std::abs(value)>=10000);
        p.drawText(QRectF(0,y-9,area.left()-8,18), Qt::AlignRight|Qt::AlignVCenter,
                   QString::number(value,scientific?'e':'f',scientific?1:1));
    }
    for (int i=0;i<7;++i) {
        const double x=area.left()+area.width()*i/6;
        p.drawText(QRectF(x-24,area.bottom()+7,48,18),Qt::AlignHCenter,QString::number(maxStep*i/6,'f',0));
    }
    double latest=values.last().step;
    const double revealStep=qMin(m_fromStep,latest)+(latest-qMin(m_fromStep,latest))*m_reveal;
    p.save();
    p.setOpacity(0.55+0.45*m_reveal);
    p.setClipRect(QRectF(area.left()-3,area.top()-3,area.width()*revealStep/maxStep+6,area.height()+6));
    for (const QString &key : {m_metric, m_metric == "loss" ? QString("eval_loss") : QString(),
                              "compare_"+m_metric, m_metric == "loss" ? QString("compare_eval_loss") : QString()}) {
        if (key.isEmpty()) continue;
        const QColor color(key.startsWith("compare_") ? Theme::comparisonSeries() :
            key == "eval_loss" && !m_comparison ? Theme::statusSuccess() : Theme::accentPrimary());
        p.setPen(QPen(color,2,key.endsWith("eval_loss") ? Qt::DashLine : Qt::SolidLine));
        QPainterPath path;
        bool first=true; double smoothed=0;
        for (const auto &s : values) if (s.key == key) {
            smoothed=first || !m_smoothed || key.endsWith("eval_loss") ? s.value : smoothed*0.45+s.value*0.55;
            Sample shown{s.step,smoothed,s.key};
            if (first) path.moveTo(point(shown)); else path.lineTo(point(shown));
            first=false;
        }
        if (key==m_metric && !path.isEmpty()) {
            auto fill=path;
            fill.lineTo(path.currentPosition().x(),area.bottom());
            const auto firstSample=std::find_if(values.begin(),values.end(),[&key](const Sample &s) { return s.key==key; });
            fill.lineTo(point(*firstSample).x(),area.bottom()); fill.closeSubpath();
            QColor top(color),bottom(color); top.setAlphaF(0.10); bottom.setAlpha(0);
            QLinearGradient gradient(area.topLeft(),area.bottomLeft());
            gradient.setColorAt(0,top); gradient.setColorAt(1,bottom);
            p.fillPath(fill,gradient);
        }
        p.drawPath(path);
        p.setBrush(color);
        if (!first) p.drawEllipse(path.currentPosition(),3,3);
        p.setBrush(Qt::NoBrush);
    }
    p.restore();
    if (m_hoverX>=area.left() && m_hoverX<=area.right()) {
        QColor cursor(Theme::textMuted()); cursor.setAlphaF(0.65); p.setPen(QPen(cursor,1,Qt::DashLine));
        p.drawLine(QPointF(m_hoverX,area.top()),QPointF(m_hoverX,area.bottom()));
    }
}

void TrainingChart::updateHover() {
    if (!m_hoverLabel) return;
    const auto area=plotRect(); const auto values=samples();
    if (values.isEmpty() || !area.contains(m_hoverPosition) || !isVisible()) {
        m_hoverLabel->hide(); m_hoverX=-1; return;
    }
    double maxStep=1; for (const auto &s : values) maxStep=qMax(maxStep,s.step);
    const double step=(m_hoverPosition.x()-area.left())/area.width()*maxStep;
    const auto nearest=std::min_element(values.begin(),values.end(),[step](const Sample &a,const Sample &b) { return std::abs(a.step-step)<std::abs(b.step-step); });
    m_hoverX=area.left()+nearest->step/maxStep*area.width();
    QStringList lines{QString("Step %1 · measured").arg(nearest->step,0,'g',6)};
    for (const QString &key : {m_metric,m_metric=="loss"?QString("eval_loss"):QString(),"compare_"+m_metric,m_metric=="loss"?QString("compare_eval_loss"):QString()}) {
        if (key.isEmpty()) continue;
        const Sample *sample=nullptr;
        for (const auto &value : values) if (value.key==key && (!sample || std::abs(value.step-step)<std::abs(sample->step-step))) sample=&value;
        if (!sample) continue;
        const QString metric=key.startsWith("compare_")?key.mid(8):key;
        QString name=metric=="loss"?"Train":metric=="eval_loss"?"Val.":metric=="learning_rate"?"LR":metric=="grad_norm"?"Gradient":"Steps/s";
        if (m_comparison) name=(key.startsWith("compare_")?"B ":"A ")+name;
        QString reading=name+"  "+QString::number(sample->value,'g',6);
        if (sample->step!=nearest->step) reading+="  @"+QString::number(sample->step,'g',6);
        lines << reading;
    }
    m_hoverLabel->setText(lines.join('\n')); m_hoverLabel->adjustSize();
    const int boxWidth=qMin(m_hoverLabel->width(),qMax(1,width()-12));
    const int boxHeight=qMin(m_hoverLabel->height(),qMax(1,height()-12));
    m_hoverLabel->resize(boxWidth,boxHeight);
    int x=qRound(m_hoverPosition.x())+12,y=qRound(m_hoverPosition.y())-boxHeight-10;
    if (x+boxWidth>width()-6) x=qRound(m_hoverPosition.x())-boxWidth-12;
    if (y<6) y=qRound(m_hoverPosition.y())+12;
    m_hoverLabel->move(qBound(6,x,qMax(6,width()-boxWidth-6)),qBound(6,y,qMax(6,height()-boxHeight-6)));
    m_hoverLabel->show(); m_hoverLabel->raise();
}

bool TrainingChart::event(QEvent *event) {
    if (event->type()==QEvent::MouseMove) {
        m_hoverPosition=static_cast<QMouseEvent *>(event)->position(); updateHover(); update();
    }
    if (event->type()==QEvent::Leave) { m_hoverX=-1; m_hoverPosition={-1,-1}; if (m_hoverLabel) m_hoverLabel->hide(); update(); }
    // Values are rendered immediately inside the chart; never open a platform tooltip.
    if (event->type()==QEvent::ToolTip) { event->accept(); return true; }
    return QWidget::event(event);
}
