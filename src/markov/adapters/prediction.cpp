#include "markov/adapters/prediction.hpp"

#include <fstream>
#include <stdexcept>
#include <utility>

namespace markov::adapters {

PreparedPrediction prepare_prediction(const std::filesystem::path & path) {
    std::ifstream input(path);
    const auto raw = nlohmann::json::parse(input);
    auto manifest = trace_graph::source::inspect_manifest(raw.at("manifest").get<std::string>());
    if (manifest.framework == "sglang") return sglang::prepare_prediction(raw, std::move(manifest));
    if (manifest.framework == "ktransformers") return ktransformers::prepare_prediction(raw, std::move(manifest));
    throw std::runtime_error("No prediction adapter for " + manifest.framework);
}

} // namespace markov::adapters
