#pragma once

#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <optional>

// What the Knowledge panel shows, read from the backend (GET /rag/status and GET /rag/embedders),
// plus the indexing-time estimate behind the "this could take hours" hint. No widgets, no constants
// about models: everything comes from the JSON, so the panel matches whatever model is configured.
namespace RagParams {

struct Preset {
    QString model, label;
    int maxTokens = 0, dim = 0, approxRamMb = 0;
    bool recommended = false;
    QString expectedDevice;                 // "cpu" or "gpu"
    QHash<QString, double> referenceSpeed;  // chunks/s by device kind, measured on referenceHardware
    QString referenceHardware;
};

struct Active {
    QString model, label, device, dtype;
    int maxTokens = 0, dim = 0;
    int chunkSize = 0, chunkOverlap = 0, maxChunkSize = 0;
    int chunks = 0, documents = 0, topK = 0;
    bool loaded = false, clamped = false, engineReady = false;
    double measuredSpeed = 0;               // 0 = not measured yet
    QString searchMode, searchRequested;    // status.search: effective and configured ("hybrid"|"vector"); empty = not reported
    bool keywordAvailable = false;          // the FTS5 keyword index can be used
};

// Pages in the "typical document" the estimate falls back to when the library is empty.
constexpr int kTypicalPages = 1000;
constexpr int kTypicalChunks = 3000;
constexpr double kSlowSeconds = 600;        // warn above ~10 minutes

QList<Preset> parseEmbedders(const QJsonArray &embedders);
std::optional<Active> parseStatus(const QJsonObject &status);   // nullopt without embedder/chunking

bool sameModel(const QString &a, const QString &b);             // tolerates the short MiniLM alias
int indexOf(const QList<Preset> &presets, const QString &model);
bool ramIsHigh(const Preset &preset, const QList<Preset> &all);  // heavier than this machine's default
double speedFor(const Preset &preset, const Active &active);    // chunks/s, 0 = unknown
QString formatDuration(double seconds);
QString searchSummary(const Active &active);                    // the one-line search mode text; empty = nothing to show
QString speedWarning(const Preset &preset, const Active &active);  // empty unless slow

}  // namespace RagParams
