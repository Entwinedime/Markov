#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include "markov/trace_graph/core/trace_event.hpp"
#include <map>
#include <span>

namespace markov::trace_graph::modules::hicache {

/** Select already-owned source calls without cutting an admitted batch at
 * HTTP end. Tail admission comes from the trace reader's causal containment,
 * never from extending the measurement window or a timestamp tolerance. */
[[nodiscard]] inline bool is_admitted_execution_call(const core::TraceEvent & event, uint64_t begin_us, uint64_t end_us) {
    return event.ts >= begin_us && (event.ts <= end_us || event.arg("formal_window_context") == "causal_tail");
}

namespace model {
struct HiCacheReplayFact;
}

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
[[nodiscard]] HiCacheExecutionBoundaries bind_hicache_execution_boundaries(core::DagGraph & graph, std::span<const model::HiCacheReplayFact> prepared = {});

/** Bind exact host-call instants to zero-duration gates in gaps, before CPU
 * entries, or within unchanged HiCache control self-time leaves. Native entry
 * gates preserve incoming dependencies without consuming the CPU operation.
 * Repeated coordinates share a gate. Self-time splitting preserves total work,
 * the original completion node, outgoing dependencies and tail gap. It does not
 * split native calls, operators, queued CPU tasks or already modeled costs. */
[[nodiscard]] std::vector<std::optional<size_t>> bind_hicache_control_points(core::DagGraph & graph, std::span<const core::TraceEvent> points);

} // namespace markov::trace_graph::modules::hicache
