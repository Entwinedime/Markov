#pragma once

#include "markov/trace_graph/source/observations.hpp"

#include <vector>

namespace markov::adapters::sglang {

std::vector<trace_graph::source::ObservedCallSequence> read_result_releases(const trace_graph::source::Observations & observed,
                                                                            const trace_graph::source::BatchResult & result, double index_element_bytes);

} // namespace markov::adapters::sglang
