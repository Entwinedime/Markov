#pragma once

#include "trace_graph/trace_dag.hpp"

#include <string>

namespace TraceGraph {

inline void apply_scale_transform(TraceDAG & dag, const std::string & name, double scale) { dag.opt_scale(name, scale); }

} // namespace TraceGraph
