#pragma once

#include "markov/trace_graph/graph/graph.hpp"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace markov::trace_graph::cost {

struct ScalingRule {
    std::string name;
    double factor;
};

// Explicit what-if assumption. Scale only a matched operation's bound service;
// first matching rule wins, leaving resources, ownership and dependencies intact.
class NodeScale {
public:
    explicit NodeScale(std::span<const ScalingRule> rules) : rules_(rules), matched_(rules.size()) {}
    bool enabled() const { return !rules_.empty(); }
    graph::Time apply(std::string_view name, graph::Time duration_ns);
    std::span<const std::size_t> matched() const { return matched_; }

private:
    std::span<const ScalingRule> rules_;
    std::vector<std::size_t> matched_;
};

} // namespace markov::trace_graph::cost
