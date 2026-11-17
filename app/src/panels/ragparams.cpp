#include "panels/ragparams.h"

#include <QLocale>

#include <cmath>

namespace RagParams {

QList<Preset> parseEmbedders(const QJsonArray &embedders) {
    QList<Preset> out;
    for (const auto &value : embedders) {
        const QJsonObject o = value.toObject();
        Preset p;
        p.model = o["model_id"].toString();
        if (p.model.isEmpty()) continue;
        p.label = o["label"].toString(p.model);
        p.maxTokens = o["max_seq_tokens"].toInt();
        p.dim = o["dim"].toInt();
        p.approxRamMb = o["approx_ram_mb"].toInt();
        p.recommended = o["recommended"].toBool();
        p.expectedDevice = o["expected_device"].toString("cpu");
        p.referenceHardware = o["reference_hardware"].toString();
        const QJsonObject speeds = o["reference_chunks_per_s"].toObject();
        for (auto it = speeds.begin(); it != speeds.end(); ++it)
            if (it.value().toDouble() > 0) p.referenceSpeed.insert(it.key(), it.value().toDouble());
        out.append(p);
    }
    return out;
}

std::optional<Active> parseStatus(const QJsonObject &status) {
    if (!status["embedder"].isObject() || !status["chunking"].isObject()) return std::nullopt;
    const QJsonObject e = status["embedder"].toObject();
    const QJsonObject c = status["chunking"].toObject();
    Active a;
    a.model = status["model"].toString(e["model"].toString());
    a.label = e["label"].toString(a.model);
    a.device = e["device"].toString();
    a.dtype = e["dtype"].toString();
    a.maxTokens = e["max_seq_tokens"].toInt();
    a.dim = e["dim"].toInt();
    a.loaded = e["loaded"].toBool();
    a.measuredSpeed = qMax(0.0, e["measured_chunks_per_s"].toDouble());
    a.chunkSize = c["chunk_size"].toInt();
    a.chunkOverlap = c["chunk_overlap"].toInt();
    a.maxChunkSize = c["max_chunk_size"].toInt();
    a.clamped = c["clamped"].toBool();
    a.chunks = status["chunks"].toInt();
    a.documents = status["documents"].toInt();
    a.topK = status["config"].toObject()["top_k"].toInt();
    a.engineReady = status["engine_ready"].toBool();
    return a;
}

bool sameModel(const QString &a, const QString &b) {
    if (a.isEmpty() || b.isEmpty()) return false;
    return a == b || a.endsWith('/' + b) || b.endsWith('/' + a);
}

int indexOf(const QList<Preset> &presets, const QString &model) {
    for (int i = 0; i < presets.size(); ++i)
        if (sameModel(presets[i].model, model)) return i;
    return -1;
}

bool ramIsHigh(const Preset &preset, const QList<Preset> &all) {
    for (const Preset &p : all)
        if (p.recommended) return preset.approxRamMb > p.approxRamMb;
    return false;
}

double speedFor(const Preset &preset, const Active &active) {
    const bool current = sameModel(preset.model, active.model);
    if (current && active.measuredSpeed > 0) return active.measuredSpeed;
    QString kind = preset.expectedDevice.isEmpty() ? QStringLiteral("cpu") : preset.expectedDevice;
    if (current && active.loaded && !active.device.isEmpty())
        kind = active.device == "cpu" ? QStringLiteral("cpu") : QStringLiteral("gpu");
    return preset.referenceSpeed.value(kind, preset.referenceSpeed.value("cpu", 0));
}

QString formatDuration(double seconds) {
    if (seconds < 60) return QStringLiteral("under a minute");
    if (seconds < 3600) {
        const int minutes = qMax(1, int(std::lround(seconds / 60)));
        return minutes == 1 ? QStringLiteral("about a minute") : QString("about %1 minutes").arg(minutes);
    }
    const int hours = int(std::lround(seconds / 3600));
    return hours <= 1 ? QStringLiteral("about an hour") : QString("about %1 hours").arg(hours);
}

QString speedWarning(const Preset &preset, const Active &active) {
    const double speed = speedFor(preset, active);
    if (speed <= 0) return {};
    // The library itself when re-indexing it is slow, otherwise the next large document.
    const bool library = active.chunks > 0 && active.chunks / speed > kSlowSeconds;
    const int chunks = library ? active.chunks : kTypicalChunks;
    const double seconds = chunks / speed;
    if (seconds <= kSlowSeconds) return {};
    const QLocale en(QLocale::English);
    const QString what = library
        ? QString("re-indexing your %1 chunks").arg(en.toString(chunks))
        : QString("%1 pages").arg(en.toString(kTypicalPages));
    QString text = QString("About %1 chunks/s here, so %2 could take %3.")
                       .arg(QString::number(speed, 'g', 2), what, formatDuration(seconds));
    if (preset.label != "Light") text += " The Light model is much faster.";
    return text;
}

}  // namespace RagParams
