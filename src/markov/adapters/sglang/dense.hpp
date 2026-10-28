#pragma once

#include "markov/adapters/probes.hpp"
#include "markov/trace_graph/planning/compute.hpp"

#include <optional>
#include <vector>

namespace markov::adapters::sglang {

struct ComputeObservation {
    trace_graph::planning::ComputeOperation operation;
    trace_graph::planning::ComputeInput input;
    std::optional<trace_graph::planning::RowBinding> rows;
};

std::vector<ComputeObservation> read_dense_work(const ProbeEvent & event);

} // namespace markov::adapters::sglang
