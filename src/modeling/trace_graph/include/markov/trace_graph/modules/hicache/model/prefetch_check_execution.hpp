#pragma once
#include "markov/trace_graph/modules/hicache/cpu_collective_timing.hpp"
#include "markov/trace_graph/modules/hicache/model/state.hpp"
#include "markov/trace_graph/modules/hicache/prefetch_control.hpp"

namespace markov::trace_graph::modules::hicache::model {

struct PrefetchCheckCpuTiming {
    std::optional<uint64_t> check_to_max, max_to_false_return, max_to_stop, best_effort_to_stop, stop_to_min, min_to_visible;
    std::optional<uint64_t> no_operation_return;
    // Zero when the caller already accounts for the outer progress prefix.
    uint64_t entry_to_check = 0;
};
// Extract only the branch actually observed. Counterfactual branch costs remain
// absent until another base/calibration call supplies them; zero is a measurement.
// Supplying the source reads service costs on unchanged observation coordinates.
// Without it, this remains a raw calibration export.
[[nodiscard]] PrefetchCheckCpuTiming observe_prefetch_check_cpu(const PrefetchControlObservation & observed,
                                                              const patch::HiCacheSourceDagIndex * source = nullptr);
struct PrefetchCheckTiming {
    std::map<int, PrefetchCheckCpuTiming> cpu;
    CpuCollectiveTiming state_max, completion_min;
};

/** One progress call over a complete TP group. Absent operations return locally;
 * active operations sample at the actual check, MAX, then stop and MIN pages.
 * A false result returns to the scheduler; this class never invents or drops
 * the ordinary work between retries. Query/physical workers run separately.
 */
class HiCachePrefetchCheckExecution {
public:
    HiCachePrefetchCheckExecution(HiCacheState & state, std::map<int, HiCacheFact> facts, PrefetchCheckTiming timing, std::string policy);
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
    PrefetchCheckTiming timing_;
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
