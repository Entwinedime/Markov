#pragma once

#include "markov/trace_graph/source/observations.hpp"

#include <memory>
#include <vector>

namespace markov::adapters::sglang {

std::unique_ptr<const trace_graph::planning::CallSequence> cache_index_copies();
std::unique_ptr<const trace_graph::planning::CallSequence> cache_backup();
std::unique_ptr<const trace_graph::planning::CallSequence> cache_index_match(double table_element_bytes);
std::vector<trace_graph::source::ObservedCallSequence> read_cache_lookup(const trace_graph::source::Observations & observed,
                                                                         const trace_graph::source::ObservedRegion & region);

} // namespace markov::adapters::sglang
