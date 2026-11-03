#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include <nlohmann/json.hpp>

#include "markov/contracts/serving.hpp"
#include "markov/graph/region_graph.hpp"

namespace markov {

struct ExecutionEvent {
    enum class Kind { Published, Completed };

    std::int64_t time_ns;
    TaskIdentity task;
    Kind kind;
};

// Serving consumes work boundaries; a node simulation and an observation supply them differently.
class ExecutionFeedback {
public:
    virtual ~ExecutionFeedback() = default;

    virtual void submit(const RegionGraph & graph, const ServingAction & action, const BatchProgram & program) = 0;
    virtual std::optional<ExecutionEvent> advance_until(std::optional<std::int64_t> deadline_ns) = 0;
    virtual void retain(std::span<const std::size_t> frontiers) = 0;
    virtual std::int64_t now_ns() const = 0;
    virtual nlohmann::json finish(bool details) const = 0;
};

} // namespace markov
