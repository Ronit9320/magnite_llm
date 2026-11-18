#include "testing/ragparams_tests.h"

#include "panels/ragparams.h"

#include <QJsonDocument>

using namespace RagParams;

namespace {

// Mirrors GET /rag/embedders on an 8 GB Apple machine (MiniLM is the default there).
QJsonArray smallMachineEmbedders(bool cudaBox = false) {
    const QString device = cudaBox ? "gpu" : "cpu";
    return QJsonDocument::fromJson(QString(R"([
      {"model_id":"sentence-transformers/all-MiniLM-L6-v2","label":"Light","max_seq_tokens":256,"dim":384,
       "approx_ram_mb":450,"recommended":%1,"expected_device":"%3","reference_hardware":"Apple M1, 8 GB",
       "reference_chunks_per_s":{"cpu":40.0}},
      {"model_id":"BAAI/bge-m3","label":"Quality","max_seq_tokens":8192,"dim":1024,"approx_ram_mb":3000,
       "recommended":%2,"expected_device":"%3","reference_hardware":"Apple M1, 8 GB",
       "reference_chunks_per_s":{"cpu":0.2,"gpu":2.0}},
      {"model_id":"Qwen/Qwen3-Embedding-0.6B","label":"Alternative","max_seq_tokens":32768,"dim":1024,
       "approx_ram_mb":3000,"recommended":false,"expected_device":"%3","reference_hardware":"Apple M1, 8 GB",
       "reference_chunks_per_s":{"cpu":0.2,"gpu":2.0}}
    ])").arg(cudaBox ? "false" : "true", cudaBox ? "true" : "false", device).toUtf8()).array();
}

QJsonObject status(const QString &model, int chunks, const QString &device = QString(), double measured = -1) {
    QJsonObject embedder{{"model", model}, {"label", "Light"}, {"max_seq_tokens", 256}, {"dim", 384},
                         {"loaded", !device.isEmpty()}, {"device", device.isEmpty() ? QJsonValue() : QJsonValue(device)},
                         {"dtype", device.isEmpty() ? QJsonValue() : QJsonValue("float32")},
                         {"measured_chunks_per_s", measured < 0 ? QJsonValue() : QJsonValue(measured)}};
    return {{"engine_ready", true}, {"model", model}, {"documents", chunks ? 2 : 0}, {"chunks", chunks},
            {"config", QJsonObject{{"chunk_size", 512}, {"chunk_overlap", 64}, {"top_k", 5}, {"model", model}}},
            {"embedder", embedder},
            {"chunking", QJsonObject{{"chunk_size", 149}, {"chunk_overlap", 37}, {"clamped", true},
                                     {"max_chunk_size", 149}}}};
}

}  // namespace

void ragParamsChecks(const std::function<void(bool, const char *)> &check) {
    const QList<Preset> presets = parseEmbedders(smallMachineEmbedders());
    check(presets.size() == 3 && presets[1].label == "Quality" && presets[1].approxRamMb == 3000 &&
              presets[1].referenceSpeed.value("gpu") == 2.0 && presets[0].recommended &&
              presets[0].referenceHardware == "Apple M1, 8 GB",
          "rag params: embedders parse label, RAM, reference speeds and the recommended flag");
    if (presets.size() != 3) return;
    const Preset &light = presets[0], &quality = presets[1];

    const auto active = parseStatus(status("all-MiniLM-L6-v2", 0));
    check(active && active->dim == 384 && active->maxTokens == 256 && active->chunkSize == 149 &&
              active->chunkOverlap == 37 && active->clamped && active->maxChunkSize == 149 &&
              active->topK == 5 && !active->loaded && active->measuredSpeed == 0,
          "rag params: status gives the effective chunking and embedder, not the stored config");
    check(!parseStatus(QJsonObject{{"model", "x"}, {"chunks", 3}}),
          "rag params: a status without embedder/chunking is rejected instead of guessed");
    if (!active) return;

    check(sameModel("all-MiniLM-L6-v2", light.model) && indexOf(presets, "all-MiniLM-L6-v2") == 0 &&
              !sameModel("BAAI/bge-m3", light.model) && indexOf(presets, "my/custom-model") == -1,
          "rag params: the short MiniLM alias matches its preset");

    const QString slow = speedWarning(quality, *active);
    check(slow.contains("0.2 chunks/s") && slow.contains("1,000 pages") && slow.contains("hours") &&
              slow.contains("Light"),
          "rag params: Quality on a CPU warns that 1,000 pages could take hours and suggests Light");
    check(speedWarning(light, *active).isEmpty(), "rag params: Light on a CPU needs no warning");

    const auto gpuLibrary = parseStatus(status("BAAI/bge-m3", 3000, "mps"));
    const auto gpuSmall = parseStatus(status("BAAI/bge-m3", 600, "mps"));
    check(gpuLibrary && speedFor(quality, *gpuLibrary) == 2.0 &&
              speedWarning(quality, *gpuLibrary).contains("3,000 chunks"),
          "rag params: a 3,000-chunk library on the GPU (25 min) warns about the library");
    check(gpuSmall && !speedWarning(quality, *gpuSmall).contains("chunks could") &&
              speedWarning(quality, *gpuSmall).contains("1,000 pages could take about 25 minutes"),
          "rag params: a quick 600-chunk library still warns that a 1,000-page document is slow");

    const auto measured = parseStatus(status("BAAI/bge-m3", 3000, "cpu", 50.0));
    check(measured && speedFor(quality, *measured) == 50.0 && speedWarning(quality, *measured).isEmpty(),
          "rag params: the measured speed overrides the reference for the active model");
    check(measured && speedFor(light, *measured) == 40.0,
          "rag params: another model's measured speed is ignored (reference used)");

    const QList<Preset> cudaPresets = parseEmbedders(smallMachineEmbedders(true));
    check(speedFor(cudaPresets[1], *active) == 2.0 && speedFor(cudaPresets[0], *active) == 40.0,
          "rag params: an unloaded model uses its expected device, falling back to the CPU figure");

    check(ramIsHigh(quality, presets) && !ramIsHigh(light, presets) && !ramIsHigh(cudaPresets[1], cudaPresets),
          "rag params: presets heavier than this machine's default are flagged for RAM");

    // Search mode (GET /rag/status "search"): hybrid, vector only, vector only because FTS5 is missing, or absent.
    const auto searchStatus = [](const QString &mode, const QString &requested, bool keyword) {
        QJsonObject s = status("all-MiniLM-L6-v2", 3);
        s["search"] = QJsonObject{{"mode", mode}, {"requested_mode", requested}, {"keyword_available", keyword}, {"rrf_k", 60}};
        return parseStatus(s);
    };
    const auto hybrid = searchStatus("hybrid", "hybrid", true);
    const auto vectorChosen = searchStatus("vector", "vector", true);
    const auto vectorForced = searchStatus("vector", "hybrid", false);
    check(hybrid && hybrid->searchMode == "hybrid" && hybrid->keywordAvailable && vectorForced &&
              vectorForced->searchMode == "vector" && !vectorForced->keywordAvailable &&
              vectorForced->searchRequested == "hybrid",
          "rag params: status gives the effective search mode and whether the keyword index is available");
    check(hybrid && searchSummary(*hybrid) == "Search: keyword + vector (hybrid)",
          "rag params: hybrid search is summarized as keyword + vector");
    check(vectorChosen && searchSummary(*vectorChosen) == "Search: vector only",
          "rag params: a vector-only choice is summarized without a reason");
    check(vectorForced && searchSummary(*vectorForced) == "Search: vector only (keyword index unavailable)",
          "rag params: hybrid requested but no FTS5 says why search is vector only");
    check(active->searchMode.isEmpty() && searchSummary(*active).isEmpty(),
          "rag params: a status without a search block keeps an empty mode and shows no line");

    check(formatDuration(30) == "under a minute" && formatDuration(25 * 60) == "about 25 minutes" &&
              formatDuration(3700) == "about an hour" && formatDuration(15000) == "about 4 hours",
          "rag params: durations read in plain language");
}
