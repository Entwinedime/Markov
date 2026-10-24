#pragma once

#include <cstdint>
#include <map>

#include <nlohmann/json.hpp>

#include "markov/graph/region_graph.hpp"

namespace markov {

// Execution consumes durations for its existing work; the provider does not change the graph.
class NodeDurations {
public:
    virtual ~NodeDurations() = default;
    virtual std::int64_t duration_ns(const RegionGraph & graph, const ExecutionNode & node) const = 0;
};

// Explicit development assumptions, with no measurements, fitting or missing-value fallback.
class GivenDurations final : public NodeDurations {
public:
    explicit GivenDurations(const nlohmann::json & values);
    std::int64_t duration_ns(const RegionGraph & graph, const ExecutionNode & node) const override;

private:
    std::map<ExecutionKind, std::int64_t> values_;
};

} // namespace markov
