#pragma once
#include "markov/trace_graph/modules/hicache/model/prefetch_check_execution.hpp"
#include "markov/trace_graph/modules/hicache/model/prefetch_scheduler_body.hpp"
#include <functional>
#include <list>
#include <memory>

namespace markov::trace_graph::modules::hicache::model {

struct PrefetchWaitTiming {
    PrefetchCheckTiming check;
    PrefetchSchedulerTiming scheduler;
};
// Immediate stop has one call per rank and a MIN, but no MAX or retry body.
[[nodiscard]] std::optional<PrefetchWaitTiming> observe_prefetch_stop_timing(const patch::HiCacheSourceDagIndex & source,
                                                                             const CpuCollectiveObservation & rounds,
                                                                             const std::map<int, std::vector<PrefetchControlObservation>> & ranks);
// One request, chronological calls per rank. No active retry returns nullopt;
// incomplete evidence for an active retry is an error, never a zero cost.
[[nodiscard]] std::optional<PrefetchWaitTiming> observe_prefetch_wait_timing(const patch::HiCacheSourceDagIndex & source,
                                                                             const CpuCollectiveObservation & rounds,
                                                                             const PrefetchWorkerObservations & workers,
                                                                             const std::map<int, std::vector<PrefetchControlObservation>> & ranks);

/** Runtime waiting loop, not a replay of source retry counts. Storage drains
 * use this same state. The caller handles receive/write/load actions, including
 * new local work joined to boundary.continuation; no default empty handler.
 */
class HiCachePrefetchWaitExecution {
public:
    using SchedulerHandler = std::function<void(PrefetchSchedulerAction, const PrefetchSchedulerBoundary &, uint64_t, simulation::FutureDag &)>;
    HiCachePrefetchWaitExecution(HiCacheState & state, std::map<int, HiCacheFact> facts, PrefetchCheckTiming check, PrefetchSchedulerTiming body,
                                 std::string policy, SchedulerHandler scheduler);
    CpuRankNodes start(const CpuRankNodes & entries, simulation::FutureDag & future);
    void advance(size_t node, uint64_t absolute_time_us, simulation::FutureDag & future);

private:
    struct Check {
        std::unique_ptr<HiCachePrefetchCheckExecution> execution;
        CpuRankNodes returned;
        size_t finished = 0;
        bool retried = false;
    };
    struct Body {
        std::map<size_t, std::map<int, HiCachePrefetchQueueSizes>> samples;
    };
    struct Action {
        PrefetchSchedulerBoundary boundary;
        std::shared_ptr<Body> run;
    };
    void issue_check(const CpuRankNodes & entries, simulation::FutureDag & future);
    void retry(const CpuRankNodes & entries, simulation::FutureDag & future);
    void dispatch(const Action & action, uint64_t time, simulation::FutureDag & future);
    HiCacheState & state_;
    std::map<int, HiCacheFact> facts_;
    PrefetchCheckTiming check_;
    PrefetchSchedulerTiming body_;
    std::string policy_;
    SchedulerHandler scheduler_;
    CpuRankNodes returned_;
    std::list<Check> checks_;
    std::unordered_map<size_t, Action> actions_;
};

} // namespace markov::trace_graph::modules::hicache::model
