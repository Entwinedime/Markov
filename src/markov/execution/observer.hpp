#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "markov/execution/timing.hpp"
#include "markov/graph/types.hpp"

namespace markov {

class CoarseGraph;

// A consumer may record actual execution without retaining historical events in the live timeline.
class ExecutionObserver {
public:
    virtual ~ExecutionObserver() = default;
    virtual void inserted(const CoarseGraph & graph, std::span<const std::size_t> events, std::span<const ServiceDuration> work_ns, std::int64_t time_ns) = 0;
    virtual void connected(std::size_t source, std::size_t target, Relation relation, std::int64_t delay_ns,
                           std::optional<std::size_t> activation) = 0;
    virtual void activated(std::size_t event, std::int64_t time_ns) = 0;
    virtual void completed(std::size_t event, std::int64_t time_ns) = 0;
    virtual void constrained(std::size_t event, std::int64_t begin_ns, std::int64_t end_ns) = 0;
};

} // namespace markov
