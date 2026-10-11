#pragma once
#include "markov/trace_graph/modules/hicache/cpu_collective_timing.hpp"
#include "markov/trace_graph/structure/hicache/scheduler.hpp"

namespace markov::trace_graph::modules::hicache::model {

// Numerical bindings: no source node IDs, event indices or timestamps.
// Observations and predictions share units, but retain separate provenance.
// Residual intervals remain separate from CPU service.
struct PrefetchSchedulerLocalTiming {
    uint64_t cpu_us = 0;
    uint64_t gap_us = 0;
    bool operator==(const PrefetchSchedulerLocalTiming &) const = default;
};
using PrefetchSchedulerLocalTimes = std::map<int, PrefetchSchedulerLocalTiming>;
struct PrefetchSchedulerStepTiming {
    CpuCollectiveTiming communication;
    PrefetchSchedulerLocalTimes before, after;
};
struct PrefetchSchedulerTiming {
    PrefetchSchedulerLocalTimes tail;
    // Numerical bindings keyed by semantic action, not an executable sequence.
    std::map<PrefetchSchedulerAction, PrefetchSchedulerStepTiming> steps;
};
struct PrefetchSchedulerBoundary {
    PrefetchSchedulerAction action;
    int rank;
    bool apply;          // false at MIN entry, true after its return/application CPU.
    size_t continuation; // Join any target-created local work here before the next step.
};
struct PrefetchSchedulerExecution {
    CpuRankNodes returned;
    std::map<size_t, PrefetchSchedulerBoundary> actions;
};
/** Emit rule-ordered work with bound costs and explicit state-action boundaries. Caller must dispatch
 * every action to its live state; this function does not assume empty queues,
 * acknowledge transfers, admit requests, or decide whether to retry.
 */
[[nodiscard]] PrefetchSchedulerExecution append_prefetch_scheduler_body(const PrefetchSchedulerTiming & body, const CpuRankNodes & entries,
                                                                        simulation::FutureDag & future);

} // namespace markov::trace_graph::modules::hicache::model
