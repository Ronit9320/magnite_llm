#include "widgets/utilizationmeter.h"

#include <QFrame>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QStyle>
#include <cmath>

UtilizationMeter::UtilizationMeter(int segmentHeight, int spacing, QWidget *parent)
    : QWidget(parent) {
    auto *row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(spacing);
    for (int i = 0; i < 32; ++i) {
        auto *segment = new QFrame(this);
        segment->setObjectName("utilizationSegment");
        segment->setMinimumWidth(1);
        segment->setFixedHeight(segmentHeight);
        row->addWidget(segment, 1);
        m_segments.append(segment);
    }
}

void UtilizationMeter::applyTelemetry(const QJsonObject &data) {
    const auto gpu = data["utilization_percent"];
    const auto cpu = data["cpu_percent"];
    m_showCpu = !gpu.isDouble() && cpu.isDouble();
    const double percent = (m_showCpu ? cpu : gpu).toDouble(-1);
    const bool measured = percent >= 0 && std::isfinite(percent);
    const double level = measured ? qMin(percent, 100.0) : 0.0;
    m_reading = measured ? QString::number(level, 'f', 0) + "%" : QStringLiteral("—");
    // Keep light activity visible even when it rounds below one segment.
    const int lit = level > 0 ? qMax(1, qRound(level * m_segments.size() / 100)) : 0;
    if (lit == m_litSegments) return;
    const int first = qMin(lit, m_litSegments);
    const int last = qMax(lit, m_litSegments);
    m_litSegments = lit;
    for (int i = first; i < last; ++i) {
        auto *segment = m_segments[i];
        segment->setProperty("lit", i < lit);
        segment->style()->unpolish(segment);
        segment->style()->polish(segment);
        segment->update();
    }
}

QString UtilizationMeter::label() const {
    return m_showCpu ? QStringLiteral("CPU utilization") : QStringLiteral("GPU utilization");
}

QString UtilizationMeter::reading() const {
    return m_reading;
}
