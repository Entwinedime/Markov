#pragma once
#include "markov/trace_graph/modules/hicache/prefetch_control.hpp"
#include "markov/trace_graph/modules/hicache/prefetch_worker_observation.hpp"

#include <cstddef>
#include <vector>

namespace markov::trace_graph::modules::hicache::patch {

struct PrefetchControlRegion {
    size_t progress_fact, entry, exit;
    int rank;
};

/** Replace complete requests' waits in one transaction, including the observed
 * scheduler bodies between progress calls. Every request/rank must end in a
 * successful progress call. Grouping avoids rebuilding a full DAG per request;
 * ordinary work between requests and their separate entry/exit gates survive.
 * Capturing observed timing before removal is the caller's responsibility.
 * Removes owned main-thread work and communication workers, not outer gaps or
 * unrelated scheduling work; all participants of a selected round are required.
 * The surviving next collective is retimed from its submission/peer arrivals;
 * its old worker idle gap must not be inherited from the removed call.
 * Returned zero-cost gates must be joined by the target execution before their
 * exits run. Boundary insertion preserves time; failure aborts the enclosing
 * workflow (the graph may already contain these harmless boundary points).
 */
[[nodiscard]] std::vector<PrefetchControlRegion> prepare_prefetch_wait_regions(core::DagGraph & graph, const std::vector<PrefetchControlObservation> & controls,
                                                                               const CpuCollectiveObservation & collectives,
                                                                               const PrefetchWorkerObservations & workers);

} // namespace markov::trace_graph::modules::hicache::patch
