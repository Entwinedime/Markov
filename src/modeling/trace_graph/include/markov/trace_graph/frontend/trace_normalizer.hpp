/**
 * @file
 * @brief Cross-channel trace normalization before DAG construction.
 */
#pragma once

#include "markov/trace_graph/core/trace_event.hpp"

#include <vector>

namespace markov::trace_graph::frontend {

/**
 * @brief Canonicalizes only fields consumed by downstream model routing.
 *
 * Source-channel provenance already belongs to the manifest and must not be copied
 * into every event. The normalizer therefore touches only recognizable HiCache
 * events, assigning the canonical category and domain used by the router.
 */
void normalize_trace_events(std::vector<core::TraceEvent> & events);

} // namespace markov::trace_graph::frontend
