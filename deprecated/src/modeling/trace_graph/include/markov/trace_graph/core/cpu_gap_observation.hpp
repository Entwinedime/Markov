/** @file Trace-supported CPU gap boundaries. */
#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include <optional>
#include <span>

namespace markov::trace_graph::core {

struct CpuGapBoundaryNodes {
    size_t begin;
    size_t end;
};

/**
 * Split a unique, unchanged CPU sequential gap at an observed interval's ends.
 * A zero-duration observation produces one point (begin == end), not an interval.
 * All time remains gap, not new execution cost; other dependencies are retained.
 * The gap is partitioned relative to the predecessor's modeled completion:
 * adjacent node costs may differ from their source durations without changing it.
 * Unsupported or ambiguous placement returns nullopt without changing the graph.
 */
[[nodiscard]] std::optional<CpuGapBoundaryNodes> insert_cpu_gap_observation(DagGraph & graph, TraceEvent observation);

/** Batch version for points only, in input order. Equal points share a node.
 * Indexes eligible gaps once; unsupported points return nullopt. Never splits
 * worker lanes with cross-thread CPU submissions, explicit worker tasks,
 * scoped/changed gaps or an existing interval envelope.
 * An observed point after a unique terminal CPU node may extend that lane's
 * tail gap; it does not acquire service cost or business endpoint eligibility.
 */
[[nodiscard]] std::vector<std::optional<size_t>> insert_cpu_gap_points(DagGraph & graph, std::span<const TraceEvent> points);

} // namespace markov::trace_graph::core
