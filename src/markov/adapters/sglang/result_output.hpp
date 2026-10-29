#pragma once

#include "markov/trace_graph/planning/native_calls.hpp"
#include "markov/trace_graph/source/observations.hpp"

#include <memory>
#include <nlohmann/json.hpp>
#include <vector>

namespace markov::adapters::sglang {

std::unique_ptr<const trace_graph::planning::CallSequence> read_result_output(const nlohmann::json & metadata, const trace_graph::source::ResultCopy & copy);
std::vector<trace_graph::source::ObservedCallSequence> read_result_callers(const trace_graph::source::Observations & observed,
                                                                           const trace_graph::source::BatchResult & result);

} // namespace markov::adapters::sglang
