#pragma once
#include "markov/trace_graph/modules/hicache/patch/io_operation_ledger.hpp"
#include "markov/trace_graph/modules/hicache/phase_observation.hpp"
#include <map>
#include <unordered_set>

namespace markov::trace_graph::modules::hicache {

/** Source attribution, separate from executable graph ownership. Keep exact
 * disjoint gap intervals: a total alone cannot be divided after CPU insertion.
 */
struct HiCacheScopeObservation {
    patch::HiCacheIoOperationLedger operations;
    HiCachePhaseObservationAudit phases;
    std::unordered_set<size_t> nodes;
    std::map<size_t, std::vector<std::pair<uint64_t, uint64_t>>> gap_intervals;
};

[[nodiscard]] HiCacheScopeObservation observe_hicache_scope(const core::DagGraph & graph);

/** Apply to the source graph, before costs/topology are rewritten. This changes
 * only the diagnostic projection, never durations, gaps or dependencies.
 * A target graph needs attribution for its actual retained/generated work;
 * source node IDs and source intervals are not a target-scope certificate.
 */
void apply_observed_hicache_scope(core::DagGraph & graph, const HiCacheScopeObservation & observation);

} // namespace markov::trace_graph::modules::hicache
