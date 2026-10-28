#pragma once

#include "markov/trace_graph/graph/graph.hpp"
#include "markov/trace_graph/planning/compute.hpp"
#include "markov/trace_graph/source/context.hpp"

#include <vector>

namespace markov::trace_graph::planning {

struct ComputeInstance {
    // A view-only call retains its dependency port without inventing a service.
    std::optional<std::size_t> work;
    source::Index source_operation;

    // Identity evidence within this source deployment, not permission to reuse
    // a CPU wall envelope or to transfer across resources/implementations.
    bool source_work_unchanged;
};

struct ComputePlan {
    std::vector<Work> work;
    std::vector<ComputeInstance> operations;

    // Local operation indices; enclosing frame/layer dependencies are separate.
    std::vector<graph::Edge> dependencies;
    std::vector<graph::Edge> queue_dependencies;
    std::vector<source::Index> missing_row_bindings;
};

// Rebind the observed compute subset to execution variables. Cache decisions and the
// backend's tensor padding rule supply these variables, never cost coefficients.
// The source remains immutable; missing row semantics do not reuse source rows.
ComputePlan project_compute(const source::SourceContext & source, source::Index frame, const FrameInput & variables);

} // namespace markov::trace_graph::planning
