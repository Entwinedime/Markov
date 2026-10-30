#pragma once

#include "markov/adapters/source.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace markov::adapters {

struct PredictionTarget {
    std::string name;
    nlohmann::json configuration;
    std::size_t page_tokens;
    std::size_t maximum_prefill_tokens;
    std::string result_implementation;
    sglang::CacheSettings cache;
};

struct PreparedPrediction {
    PreparedSource source;
    SelectedCosts costs;
    std::filesystem::path models_path;
    trace_graph::planning::KernelCacheInitial initial;
    std::vector<PredictionTarget> targets;
};

// Only declared base, workload and independent models enter prediction.
PreparedPrediction prepare_prediction(const std::filesystem::path & path);

} // namespace markov::adapters
