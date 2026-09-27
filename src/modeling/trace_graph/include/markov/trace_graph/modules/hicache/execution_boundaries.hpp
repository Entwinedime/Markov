#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include <map>
#include <span>

namespace markov::trace_graph::modules::hicache {

namespace model { struct HiCacheReplayFact; }

struct HiCacheExecutionBoundaries {
    // Semantic fact IDs and executable node IDs are different namespaces.
    std::map<size_t, size_t> fact_nodes;
    std::map<size_t, std::vector<std::string>> issues;
};

/** Bind approved formal/tail state facts to zero-cost points in unchanged CPU
 * gaps. Prelude remains initial state. Reuses points on repeated calls; missing
 * execution evidence stays explicit, never a nearest-node or source-time fallback.
 * This prepares the source skeleton; it does not simulate target cache decisions.
 */
[[nodiscard]] HiCacheExecutionBoundaries bind_hicache_execution_boundaries(
    core::DagGraph & graph, std::span<const model::HiCacheReplayFact> prepared = {});

/** Bind exact host-call instants in gaps, at CPU entries, or within unchanged
 * HiCache control self-time leaves. Self-time splitting preserves total work,
 * the original completion node, outgoing dependencies and tail gap. It does not
 * split native calls, operators, queued CPU tasks or already modeled costs. */
[[nodiscard]] std::vector<std::optional<size_t>> bind_hicache_control_points(core::DagGraph & graph, std::span<const core::TraceEvent> points);

} // namespace markov::trace_graph::modules::hicache
