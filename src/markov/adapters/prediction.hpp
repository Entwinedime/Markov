#pragma once

#include "markov/adapters/source.hpp"
#include "markov/trace_graph/planning/cache_program.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace markov::adapters {

struct PredictionTarget {
    std::string name;
    nlohmann::json configuration;
    nlohmann::json deployment;
    nlohmann::json work_layout;
    trace_graph::planning::CacheBackend backend;
    std::size_t page_tokens;
    trace_graph::planning::RegionBackend regions;
    sglang::CacheSettings cache;
    std::optional<std::string> infeasible = std::nullopt;
};

struct PreparedPrediction {
    PreparedSource source;
    nlohmann::json deployment;
    std::map<std::size_t, SelectedCosts> costs;
    std::map<std::size_t, std::filesystem::path> models_paths;
    trace_graph::planning::KernelCacheInitial initial;
    std::vector<PredictionTarget> targets;
    std::vector<trace_graph::cost::ScalingRule> node_scale;
    bool record_graph;
};

// Only declared base, workload and independent models enter prediction.
PreparedPrediction prepare_prediction(const std::filesystem::path & path);

} // namespace markov::adapters
