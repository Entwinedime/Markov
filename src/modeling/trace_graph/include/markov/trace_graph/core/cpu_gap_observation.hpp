/** @file Trace-supported interval boundaries inside an existing CPU gap. */
#pragma once

#include "markov/trace_graph/core/dag_graph.hpp"
#include <optional>

namespace markov::trace_graph::core {

struct CpuGapBoundaryNodes {
    size_t begin;
    size_t end;
};

/**
 * Split a unique, unchanged CPU sequential gap at an observed interval's ends.
 * A zero-duration observation produces one point (begin == end), not an interval.
 * All time remains gap, not new execution cost; other dependencies are retained.
 * Unsupported or ambiguous placement returns nullopt without changing the graph.
 */
[[nodiscard]] std::optional<CpuGapBoundaryNodes> insert_cpu_gap_observation(DagGraph & graph, TraceEvent observation);

} // namespace markov::trace_graph::core
