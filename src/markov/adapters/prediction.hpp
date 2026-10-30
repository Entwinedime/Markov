#pragma once

#include "markov/adapters/source.hpp"
#include "markov/trace_graph/planning/serial_program.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace markov::adapters {

struct PredictionTarget {
    std::string name;
    nlohmann::json configuration;
    std::size_t page_tokens;
    std::size_t maximum_prefill_tokens;
    std::string result_implementation;
    sglang::CacheSettings cache;
    std::optional<std::string> infeasible = std::nullopt;
};

struct SerialPredictionTarget {
    std::string name;
    nlohmann::json configuration;
    std::vector<trace_graph::planning::SerialRequest> requests;
    std::optional<std::string> infeasible;
};

struct PreparedPrediction {
    PreparedSource source;
    SelectedCosts costs;
    std::filesystem::path models_path;
    trace_graph::planning::KernelCacheInitial initial;
    std::variant<std::vector<PredictionTarget>, std::vector<SerialPredictionTarget>> targets;
    std::vector<trace_graph::cost::ScalingRule> node_scale;
    bool record_graph;
};

// Only declared base, workload and independent models enter prediction.
PreparedPrediction prepare_prediction(const std::filesystem::path & path);

} // namespace markov::adapters
