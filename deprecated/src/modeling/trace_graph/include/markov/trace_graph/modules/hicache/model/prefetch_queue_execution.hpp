#pragma once
#include "markov/trace_graph/modules/hicache/cpu_collective_timing.hpp"
#include "markov/trace_graph/modules/hicache/model/state.hpp"

namespace markov::trace_graph::modules::hicache::model {

struct PrefetchQueueCpuTiming {
    uint64_t before_sample, sample_to_min, min_to_apply;
};
struct PrefetchQueueTiming {
    std::map<int, PrefetchQueueCpuTiming> cpu;
    CpuCollectiveTiming agreement;
};

/** One query over a complete TP group: sample local hits, take their MIN,
 * then apply at each rank's return. Facts identify the original candidates.
 * Local CPU delays are measured inputs, not inferred from target outcomes.
 * Scheduler drains belong to the foreground wait/confirmation execution.
 */
class HiCachePrefetchQueueExecution {
public:
    HiCachePrefetchQueueExecution(HiCacheState & state, std::map<int, HiCacheFact> facts, PrefetchQueueTiming timing);
    // Call before the earliest entry. Caller serializes query-thread entries
    // using these return gates, independently of payload-worker completion.
    CpuRankNodes start(const CpuRankNodes & entries, simulation::FutureDag & future);
    // Query inspects its candidate at each rank's entry, after the caller has
    // applied that candidate. An absent candidate creates no background work.
    // A returned rank has applied its result (or skipped an absent candidate);
    // the caller can then enqueue it on HiCachePrefetchExecution.
    std::optional<int> advance(size_t node, uint64_t absolute_time_us, simulation::FutureDag & future);

private:
    enum class Step { Entry, Sample, Apply, Finished };
    struct Event {
        int rank;
        Step step;
    };
    size_t boundary(int rank, Step step, size_t previous, uint64_t delay, simulation::FutureDag & future);
    void enter(int rank, size_t node, const HiCacheFact & fact, simulation::FutureDag & future);
    HiCacheState & state_;
    std::map<int, HiCacheFact> facts_;
    PrefetchQueueTiming timing_;
    std::map<int, uint64_t> samples_;
    std::unordered_map<size_t, Event> events_;
    CpuRankNodes owners_, entries_, returned_;
    std::optional<CpuCollectiveExecution> agreement_;
    std::optional<bool> participating_;
};

} // namespace markov::trace_graph::modules::hicache::model
