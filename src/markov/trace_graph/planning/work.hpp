#pragma once

#include <compare>
#include <map>
#include <string>

namespace markov::trace_graph::planning {

using Features = std::map<std::string, double>;

struct WorkKind {
    std::string operation;
    std::string implementation;
    std::string timing_scope;

    auto operator<=>(const WorkKind &) const = default;
};

// A work query describes service, not its queueing delay or predicted duration.
// Deployment and implementation conditions belong to the selected model catalog.
struct Work {
    WorkKind kind;
    Features features;

    auto operator<=>(const Work &) const = default;
};

} // namespace markov::trace_graph::planning
