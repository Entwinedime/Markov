#pragma once

#include "markov/trace_graph/cost/models.hpp"
#include "markov/trace_graph/cost/scaling.hpp"
#include "markov/trace_graph/source/cpu_costs.hpp"
#include "markov/trace_graph/source/manifest.hpp"

#include <filesystem>
#include <string>
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

struct SourceCpuCosts {
    std::filesystem::path path;
    std::string basis;
    std::vector<std::string> sources;
    trace_graph::source::CpuCosts intervals;
};

// JSON is resolved and validated here once. The cost core receives only typed
// work, coefficients and coordinates; it never opens the source captures.
SelectedCosts read_cost_models(const std::filesystem::path & path);
std::vector<trace_graph::cost::ScalingRule> read_node_scale(const nlohmann::json & raw);
CostQueries read_cost_queries(const std::filesystem::path & path);
SourceCpuCosts read_source_cpu_costs(const std::filesystem::path & path, const trace_graph::source::ProfileManifest & manifest,
                                     const trace_graph::source::Observations & observations);

} // namespace markov::adapters
