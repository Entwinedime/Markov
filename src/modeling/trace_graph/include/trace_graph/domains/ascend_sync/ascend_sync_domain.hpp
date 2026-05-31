#pragma once

#include "trace_graph/trace_dag.hpp"

namespace TraceGraph {

inline void apply_ascend_sync_domain(TraceDAG & dag) {
    dag.joinEvent();
    dag.blockinganalysis();
    dag.joinSync();
}

} // namespace TraceGraph
