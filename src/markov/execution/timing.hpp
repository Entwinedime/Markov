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

// The caller has already matched this invocation's boundary; execution never matches source and target ordinals.
struct BoundaryTime {
    std::size_t event;
    TimeWindow window;
};

class CoarseGraph;

struct GraphTiming {
    std::vector<ServiceDuration> work_ns;
    std::vector<BoundaryTime> boundaries;

    // Unknown work needs a causal or explicitly observed completion bound; neither supplies its service cost.
    static GraphTiming conditioned(const CoarseGraph & graph, std::vector<ServiceDuration> work_ns, std::vector<BoundaryTime> boundaries,
                                   std::optional<TimeWindow> unmeasured_completion_window = std::nullopt);
};

struct EventTiming {
    ServiceDuration service_ns;
    std::optional<TimeWindow> window;
};

} // namespace markov
