#pragma once

#include "markov/trace_graph/modules/hicache/model/prefetch_scheduler_body.hpp"
#include "markov/trace_graph/modules/hicache/prefetch_control.hpp"
#include "markov/trace_graph/modules/hicache/prefetch_worker_observation.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <vector>

namespace markov::trace_graph::source_dag {

using modules::hicache::CpuCollectiveObservation;
using modules::hicache::CpuCollectiveTiming;
using modules::hicache::PrefetchControlObservation;
using modules::hicache::PrefetchWorkerObservations;
using modules::hicache::model::PrefetchSchedulerAction;
using modules::hicache::model::PrefetchSchedulerTiming;
namespace patch = modules::hicache::patch;

/** All complete query groups in the requested window. Per-call identity is
 * retained; no first-call template or cross-call averaging. Intervals include
 * scheduling/communication residuals and only apply CPU correction where the
 * source supplies measured spans. They are not pure CPU service samples. */
[[nodiscard]] nlohmann::json prefetch_query_observations(const core::DagGraph & graph, const CpuCollectiveObservation & collectives,
                                                         const PrefetchWorkerObservations & workers, uint64_t begin_us, uint64_t end_us);

struct PrefetchCheckCpuTiming {
    std::optional<uint64_t> check_to_max, max_to_false_return, max_to_stop, best_effort_to_stop, stop_to_min, min_to_visible;
    std::optional<uint64_t> no_operation_return;
    // Zero when the caller already accounts for the outer progress prefix.
    uint64_t entry_to_check = 0;
};
// Extract only the branch actually observed. Counterfactual branch costs remain
// absent in this observation; independent samples train their cost models.
// A measured zero remains distinct from an unobserved branch.
// Reads service costs on unchanged observation coordinates from the same graph.
[[nodiscard]] PrefetchCheckCpuTiming observe_prefetch_check_cpu(const PrefetchControlObservation & observed,
                                                                const patch::HiCacheSourceDagIndex & source);
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

[[nodiscard]] PrefetchSchedulerTiming prefetch_scheduler_timing(const PrefetchSchedulerBody & observed, const core::DagGraph & graph);

/** One source loop body, from a false progress return to the next progress
 * entry. Captures timing/shape, never queue values or target retry counts.
 * Observation alone does not authorize removal of external graph dependencies.
 */
[[nodiscard]] PrefetchSchedulerBody observe_prefetch_scheduler_body(const patch::HiCacheSourceDagIndex & source, const CpuCollectiveObservation & collectives,
                                                                    const PrefetchWorkerObservations & workers,
                                                                    const std::vector<PrefetchControlObservation> & previous,
                                                                    const std::vector<PrefetchControlObservation> & next);

} // namespace markov::trace_graph::source_dag
