#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace markov {

// Unknown service is retained as unknown; execution then requires an explicit completion condition.
using ServiceDuration = std::optional<std::int64_t>;

// A measured interval permits a causal realization, not an asserted exact timestamp.
struct TimeWindow {
    std::int64_t begin_ns;
    std::int64_t end_ns;
};

// A native witness without a window leaves readiness to the graph's dependencies.
struct BoundaryCondition {
    std::size_t event;
    std::optional<TimeWindow> window;
    std::optional<std::size_t> source_event{};
};

class CoarseGraph;

struct GraphTiming {
    std::vector<ServiceDuration> work_ns;
    std::vector<BoundaryCondition> boundaries;

    // Unknown work needs its own condition or a bound from an actual causal successor; neither supplies its service cost.
    static GraphTiming conditioned(const CoarseGraph & graph, std::vector<ServiceDuration> work_ns, std::vector<BoundaryCondition> boundaries);
};

struct EventTiming {
    ServiceDuration service_ns;
    std::optional<TimeWindow> window;
};

} // namespace markov
