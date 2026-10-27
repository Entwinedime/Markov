/**
 * @file
 * @brief Cross-channel trace normalization implementation.
 */
#include "markov/trace_graph/frontend/trace_normalizer.hpp"
#include "markov/trace_graph/adapters/frameworks/sglang/trace_semantics.hpp"

namespace markov::trace_graph::frontend {

void normalize_trace_events(std::vector<core::TraceEvent> & events) {
    for (auto & event : events) { adapters::sglang::classify_event(event); }
}

} // namespace markov::trace_graph::frontend
