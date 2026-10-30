#pragma once

#include "markov/adapters/costs.hpp"
#include "markov/adapters/source.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace markov::adapters {

struct PreparedReplay {
    PreparedSource source;
    std::vector<trace_graph::cost::ScalingRule> node_scale;
    std::map<trace_graph::graph::NodeId, trace_graph::graph::Time> overrides;
    std::optional<std::string> inspect_name;
};

// Diagnostic only: explicit costs replace nodes in the selected source DAG.
// This does not map a target profile or regenerate conditional execution.
PreparedReplay prepare_replay(const std::filesystem::path & path);

} // namespace markov::adapters
