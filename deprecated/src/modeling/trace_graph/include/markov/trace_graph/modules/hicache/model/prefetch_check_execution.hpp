#pragma once
#include "markov/trace_graph/modules/hicache/cpu_collective_timing.hpp"
#include "markov/trace_graph/modules/hicache/model/state.hpp"
#include <functional>

namespace markov::trace_graph::modules::hicache::model {

// Resolve only the interval reached by target execution. Durations are in us;
// an unavailable independent cost propagates as MissingCostEvidence.
using PrefetchCheckLocalCost = std::function<uint64_t(int, std::string_view)>;
// Numerical fields only; the executor supplies participant dependencies.
using PrefetchCheckCollectiveCost = std::function<CpuCollectiveCallTiming(int, bool)>;

/** One progress call over a complete TP group. Absent operations return locally;
 * active operations sample at the actual check, MAX, then stop and MIN pages.
 * A false result returns to the scheduler; this class never invents or drops
 * the ordinary work between retries. Query/physical workers run separately.
 */
class HiCachePrefetchCheckExecution {
public:
    HiCachePrefetchCheckExecution(HiCacheState & state, std::map<int, HiCacheFact> facts, std::string policy, PrefetchCheckLocalCost local_cost,
                                  PrefetchCheckCollectiveCost collective_cost);
    // Invoke no later than the earliest zero-cost entry. Returned nodes are
    // completion gates; callers may attach their unstarted continuations.
    CpuRankNodes start(const CpuRankNodes & entries, simulation::FutureDag & future);
    // At a returned gate: true means no pending prefetch remains on that rank;
    // false means the scheduler must retry. Other nodes return nullopt.
    std::optional<bool> advance(size_t node, uint64_t absolute_time_us, simulation::FutureDag & future);

private:
    enum class Step { Entry, Sample, MaxReturned, Stop, Visible, Finished };
    struct Event {
        int rank;
        Step step;
    };
    size_t boundary(int rank, Step step, size_t predecessor, uint64_t delay, simulation::FutureDag & future);
    void enter(int rank, size_t node, const HiCacheFact & fact, simulation::FutureDag & future);
    size_t stop_entry(int rank, size_t predecessor, bool immediate, simulation::FutureDag & future);
    void terminal(const CpuRankNodes & predecessors, bool immediate, simulation::FutureDag & future);
    HiCacheState & state_;
    std::map<int, HiCacheFact> facts_;
    CpuCollectiveTiming collective(bool completion) const;
    PrefetchCheckLocalCost local_cost_;
    PrefetchCheckCollectiveCost collective_cost_;
    bool immediate_;
    CpuRankNodes owners_, entries_, returned_, max_returned_;
    std::optional<CpuCollectiveExecution> first_collective_;
    std::optional<bool> participating_;
    std::map<int, HiCachePrefetchCheck> samples_;
    std::map<int, uint64_t> stopped_;
    std::unordered_map<size_t, Event> events_;
    std::optional<bool> allowed_;
};

} // namespace markov::trace_graph::modules::hicache::model
