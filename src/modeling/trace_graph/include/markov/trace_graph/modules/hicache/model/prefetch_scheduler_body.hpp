#pragma once
#include "markov/trace_graph/modules/hicache/prefetch_control.hpp"
#include "markov/trace_graph/modules/hicache/prefetch_worker_observation.hpp"

namespace markov::trace_graph::modules::hicache::model {

enum class PrefetchSchedulerAction { Receive, WriteCompletion, LoadCompletion, StorageDrain };
struct PrefetchSchedulerStep {
    PrefetchSchedulerAction action;
    size_t source_round;
    CpuCollectiveTiming communication;
    // Ordinary CPU and residual time, excluding collective calls. Not a pure
    // CPU-service calibration. after includes the observed confirmation/drain.
    std::map<int, patch::HiCacheTimingIntervalOwnership> before, after;
    // Absent in old captures: a zero after interval then does NOT prove that
    // confirmation/release is free or that it occurs at collective return.
    std::map<int, size_t> completion_observations;
};
struct PrefetchSchedulerBody {
    std::map<int, patch::HiCacheTimingIntervalOwnership> lanes, tail;
    std::vector<PrefetchSchedulerStep> steps;
    std::string issue;
};

// Portable measured work: no source node IDs, event indices or timestamps.
// Includes residual intervals; these are not pure CPU service measurements.
struct PrefetchSchedulerLocalTiming {
    uint64_t cpu_us = 0;
    uint64_t gap_us = 0;
    bool operator==(const PrefetchSchedulerLocalTiming &) const = default;
};
using PrefetchSchedulerLocalTimes = std::map<int, PrefetchSchedulerLocalTiming>;
struct PrefetchSchedulerStepTiming {
    PrefetchSchedulerAction action;
    CpuCollectiveTiming communication;
    PrefetchSchedulerLocalTimes before, after;
};
struct PrefetchSchedulerTiming {
    PrefetchSchedulerLocalTimes tail;
    std::vector<PrefetchSchedulerStepTiming> steps;
};
[[nodiscard]] PrefetchSchedulerTiming prefetch_scheduler_timing(const PrefetchSchedulerBody & observed, const core::DagGraph & graph);

/** One source loop body, from a false progress return to the next progress
 * entry. Captures timing/shape, never queue values or target retry counts.
 * Observation alone does not authorize removal of external graph dependencies.
 */
[[nodiscard]] PrefetchSchedulerBody observe_prefetch_scheduler_body(const patch::HiCacheSourceDagIndex & source, const CpuCollectiveObservation & collectives,
                                                                    const PrefetchWorkerObservations & workers,
                                                                    const std::vector<PrefetchControlObservation> & previous,
                                                                    const std::vector<PrefetchControlObservation> & next);

struct PrefetchSchedulerBoundary {
    size_t step;
    int rank;
    bool apply;          // false at MIN entry, true after its return/application CPU.
    size_t continuation; // Join any target-created local work here before the next step.
};
struct PrefetchSchedulerExecution {
    CpuRankNodes returned;
    std::map<size_t, PrefetchSchedulerBoundary> actions;
};
/** Emit measured work and explicit state-action boundaries. Caller must dispatch
 * every action to its live state; this function does not assume empty queues,
 * acknowledge transfers, admit requests, or decide whether to retry.
 */
[[nodiscard]] PrefetchSchedulerExecution append_prefetch_scheduler_body(const PrefetchSchedulerTiming & body, const CpuRankNodes & entries,
                                                                        simulation::FutureDag & future);

} // namespace markov::trace_graph::modules::hicache::model
