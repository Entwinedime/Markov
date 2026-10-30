#pragma once

#include "markov/trace_graph/source/services.hpp"

#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace markov::trace_graph::source {

struct NativeLaunchPorts {
    std::optional<ObservedRegion> worker_before, device;
};

struct NativeQueuePorts {
    std::optional<ObservedRegion> dispatch, worker_before, worker_after;
    Time excluded_queue_wait_ns = 0;
    std::vector<NativeLaunchPorts> launches;
    bool code_preparation = false;
};

// Source captures and independent sampling share one interpretation of queue
// dispatch, native launch entries and exclusive device ownership.
class NativeQueues {
public:
    NativeQueues(const Observations & observed, const SourceGraph & graph);
    NativeQueuePorts ports(Index queue, const planning::NativeCall & definition) const;

private:
    const Observations & observed_;
    std::vector<std::optional<Time>> previous_worker_end_;
    std::vector<std::vector<Index>> runtime_calls_, submissions_, controls_;
    std::vector<Time> exclusive_service_;
    std::map<std::pair<Index, Time>, std::optional<Index>> device_spans_;
};

} // namespace markov::trace_graph::source
