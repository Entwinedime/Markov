#pragma once

#include "markov/trace_graph/cost/models.hpp"

#include <filesystem>
#include <vector>

#include <nlohmann/json.hpp>

namespace markov::adapters {

struct SelectedCosts {
    nlohmann::json deployment;
    trace_graph::cost::Models models;
};

struct CostQueries {
    std::filesystem::path models_path;
    nlohmann::json deployment;
    std::vector<trace_graph::planning::Work> work;
};

// JSON is resolved and validated here once. The cost core receives only typed
// work, coefficients and coordinates; it never opens the source captures.
SelectedCosts read_cost_models(const std::filesystem::path & path);
CostQueries read_cost_queries(const std::filesystem::path & path);

} // namespace markov::adapters
