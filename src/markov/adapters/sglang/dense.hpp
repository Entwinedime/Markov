#pragma once

#include "markov/adapters/probes.hpp"
#include "markov/trace_graph/planning/compute.hpp"
#include "markov/trace_graph/planning/native_calls.hpp"

#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace markov::adapters::sglang {

struct ComputeObservation {
    trace_graph::planning::ComputeOperation operation;
    trace_graph::planning::ComputeInput input;
    std::optional<trace_graph::planning::RowBinding> rows;
};

std::vector<ComputeObservation> read_dense_work(const ProbeEvent & event);
struct ComputeCalls {
    trace_graph::planning::Features parameters;
    std::unique_ptr<const trace_graph::planning::CallSequence> definition;
};
ComputeCalls read_dense_calls(const ProbeEvent & event, std::span<const trace_graph::planning::ComputePart> recipe);

} // namespace markov::adapters::sglang
