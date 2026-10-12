#pragma once
#include "markov/trace_graph/modules/hicache/model/prefetch_check_execution.hpp"
#include "markov/trace_graph/modules/hicache/model/prefetch_scheduler_body.hpp"
#include <functional>
#include <list>
#include <memory>

namespace markov::trace_graph::modules::hicache::model {

/** Runtime waiting loop, not a replay of source retry counts. Storage drains
 * use this same state. The caller handles receive/write/load actions, including
 * new local work joined to boundary.continuation; no default empty handler.
 */
class HiCachePrefetchWaitExecution {
public:
    using SchedulerHandler = std::function<void(const PrefetchSchedulerBoundary &, uint64_t, simulation::FutureDag &)>;
    using SchedulerCosts = std::function<PrefetchSchedulerTiming()>;
    HiCachePrefetchWaitExecution(HiCacheState & state, std::map<int, HiCacheFact> facts, SchedulerCosts scheduler_costs, std::string policy,
                                 PrefetchCheckLocalCost local_cost, PrefetchCheckCollectiveCost collective_cost, SchedulerHandler scheduler);
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
        std::map<int, HiCachePrefetchQueueSizes> storage_samples;
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
    SchedulerCosts scheduler_costs_;
    std::string policy_;
    PrefetchCheckLocalCost local_cost_;
    PrefetchCheckCollectiveCost collective_cost_;
    SchedulerHandler scheduler_;
    CpuRankNodes returned_;
    std::list<Check> checks_;
    std::unordered_map<size_t, Action> actions_;
};

} // namespace markov::trace_graph::modules::hicache::model
