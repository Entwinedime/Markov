#pragma once

#include "trace_graph/trace_dag.hpp"

namespace TraceGraph {

inline void run_topological_simulation(TraceDAG & dag) { dag.simulation(); }

} // namespace TraceGraph
