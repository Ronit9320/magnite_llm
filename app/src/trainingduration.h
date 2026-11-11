#pragma once

#include <QString>
#include <cmath>
#include <limits>

namespace TrainingTime {
// Keep training estimates and elapsed time compact even for runs lasting days.
inline QString duration(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0 ||
        seconds >= static_cast<double>(std::numeric_limits<qint64>::max())) return "—";
    const auto total = static_cast<qint64>(seconds);
    if (total >= 86400)
        return QString("%1d %2h").arg(total / 86400).arg(total % 86400 / 3600);
    if (total >= 3600)
        return QString("%1h %2m").arg(total / 3600).arg(total % 3600 / 60);
    if (total >= 60)
        return QString("%1m %2s").arg(total / 60).arg(total % 60);
    return QString("%1s").arg(total);
}
} // namespace TrainingTime
