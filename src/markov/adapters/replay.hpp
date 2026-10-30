#pragma once

#include "markov/adapters/costs.hpp"
#include "markov/adapters/source.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace markov::adapters {

struct PredictedTrajectory {
    std::filesystem::path prediction;
    nlohmann::json cell;
    nlohmann::json deployment;
    std::filesystem::path source_manifest;
};

struct PreparedReplay {
    std::variant<PreparedSource, PredictedTrajectory> input;
    std::vector<trace_graph::cost::ScalingRule> node_scale;
    std::map<trace_graph::graph::NodeId, trace_graph::graph::Time> overrides;
    std::optional<std::string> inspect_name;
};

// Diagnostic only: replace costs in a source DAG or recorded generated path.
// Neither mode maps a target profile or reselects conditional execution.
PreparedReplay prepare_replay(const std::filesystem::path & path);
PredictedTrajectory read_trajectory(const std::filesystem::path & prediction, const std::string & target);

} // namespace markov::adapters
