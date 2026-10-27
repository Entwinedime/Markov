#pragma once

#include "markov/trace_graph/planning/compute.hpp"
#include "markov/trace_graph/source/context.hpp"

#include <vector>

namespace markov::trace_graph::planning {

struct ComputeInstance {
    std::size_t work;
    source::Index source_operation;
};

struct ComputePlan {
    std::vector<Work> work;
    std::vector<ComputeInstance> operations;
    std::vector<source::Index> missing_row_bindings;
};

// Rebind the observed compute subset to execution variables. Cache decisions and the
// backend's tensor padding rule supply these variables, never cost coefficients.
// The source remains immutable; missing row semantics do not reuse source rows.
ComputePlan project_compute(const source::SourceContext & source, source::Index frame, const AttentionInput & variables);

} // namespace markov::trace_graph::planning
